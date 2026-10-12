#include <glib.h>
#include <errno.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "config.h"
#include "database.h"
#include "process_directories.h"
#include "queue.h"
#include "process_file.h"
#include "version.h"
#include "logging.h"
#include "summary.h"
#include "db_writer.h"
#include "io_sched.h"
#include "storage.h"
#include "file_task.h"

// Exit codes
#define EXIT_OK              0
#define EXIT_CHANGES_FOUND   1
#define EXIT_ERROR           2


void
show_help(const gchar *prog_name)
{
    g_print ("Project URL: https://github.com/paolostivanin/FastFileCheck\n\n");
    g_print ("Usage:\n");
    g_print ("  %s [OPTIONS] COMMAND\n\n", prog_name);
    g_print ("Commands:\n");
    g_print ("  add     Add files to the database\n");
    g_print ("  check   Check files against the database\n");
    g_print ("  update  Remove/update files in the database\n");
    g_print ("  stats   Show database statistics\n\n");
    g_print ("Options:\n");
    g_print ("  -h, --help      Show this help message and exit\n");
    g_print ("  -v, --version   Show version information and exit\n");
    g_print ("  -c, --config    Path to config file (default: /etc/ffc.conf)\n");
    g_print ("  -V, --verbose   Verbose output with heartbeat/progress\n");
    g_print ("  -j, --json      Output results as JSON\n");
    g_print ("  --full          Force full re-hash (disable quick mtime+size skip)\n");
}


static void
worker_thread (gpointer data,
               gpointer user_data)
{
    FileTask *task = (FileTask *)data;
    ConsumerData *consumer_data = (ConsumerData *)user_data;
    process_file (task, consumer_data);
    io_sched_release (consumer_data->sched, task->charged_bytes);
    file_task_free (task);
}


static gpointer
queue_consumer(gpointer data)
{
    ConsumerData *consumer_data = (ConsumerData *)data;
    io_sched_run (consumer_data->sched);
    return NULL;
}


// Idle class: our reads only get the disk when nobody else wants it. ioprio is per thread and
// inherited by threads created afterwards, so this must run before any thread is started.
static void
set_idle_io_priority (void)
{
#ifdef SYS_ioprio_set
    enum { IOPRIO_WHO_PROCESS_ = 1, IOPRIO_CLASS_IDLE_ = 3, IOPRIO_CLASS_SHIFT_ = 13 };
    if (syscall (SYS_ioprio_set, IOPRIO_WHO_PROCESS_, 0, IOPRIO_CLASS_IDLE_ << IOPRIO_CLASS_SHIFT_) != 0) {
        g_warning ("Could not set the idle I/O priority: %s", g_strerror (errno));
    }
#else
    g_warning ("low_priority is not supported on this platform");
#endif
}


// Split `directories` on commas, trim blanks and trailing slashes, drop empty entries
static gchar **
parse_directories (const gchar *list)
{
    gchar **dirs = g_strsplit (list, ",", -1);
    gsize count = 0;
    for (gsize i = 0; dirs[i] != NULL; i++) {
        g_strstrip (dirs[i]);
        gsize len = strlen (dirs[i]);
        while (len > 1 && dirs[i][len - 1] == '/') dirs[i][--len] = '\0';
        if (len > 0) dirs[count++] = dirs[i];
        else g_free (dirs[i]);
    }
    dirs[count] = NULL;
    return dirs;
}

static gpointer
progress_reporter (gpointer data)
{
    ConsumerData *consumer_data = (ConsumerData *)data;
    // Periodically report progress until work is done
    while (TRUE) {
        g_usleep (2 * 1000 * 1000);
        guint qlen = (guint)MAX (g_async_queue_length (consumer_data->file_queue_data->queue), 0);
        gboolean done = g_atomic_int_get (&consumer_data->file_queue_data->scanning_done);
        guint unprocessed = g_thread_pool_unprocessed (consumer_data->thread_pool);
        g_message ("Progress: processed=%u, queue=%u, pending=%u, scanning_done=%s",
                   summary_get_processed (consumer_data->summary_data),
                   qlen,
                   unprocessed,
                   done ? "yes" : "no");
        if (done && qlen == 0 && unprocessed == 0) break;
    }
    return NULL;
}


int
main (int argc, char *argv[])
{
    // Basic option parsing: allow --help/--version/--config PATH/--verbose/--json/--full before COMMAND
    const char *config_path = NULL;
    gboolean verbose_flag = FALSE;
    gboolean json_flag = FALSE;
    gboolean full_flag = FALSE;

    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (g_strcmp0 (argv[i], "-h") == 0 || g_strcmp0 (argv[i], "--help") == 0) {
            show_help (argv[0]);
            return EXIT_OK;
        } else if (g_strcmp0 (argv[i], "-v") == 0 || g_strcmp0 (argv[i], "--version") == 0) {
            g_print ("%s v%s\n", argv[0], FASTFILECHECK_VERSION_FULL);
            return EXIT_OK;
        } else if (g_strcmp0 (argv[i], "-c") == 0 || g_strcmp0 (argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                show_help (argv[0]);
                return EXIT_ERROR;
            }
            config_path = argv[i + 1];
            i += 2;
            continue;
        } else if (g_strcmp0 (argv[i], "-V") == 0 || g_strcmp0 (argv[i], "--verbose") == 0) {
            verbose_flag = TRUE;
            i++;
            continue;
        } else if (g_strcmp0 (argv[i], "-j") == 0 || g_strcmp0 (argv[i], "--json") == 0) {
            json_flag = TRUE;
            i++;
            continue;
        } else if (g_strcmp0 (argv[i], "--full") == 0) {
            full_flag = TRUE;
            i++;
            continue;
        } else {
            break;
        }
    }

    if (i >= argc) {
        show_help (argv[0]);
        return EXIT_ERROR;
    }

    const char *command = argv[i];

    ConfigData *config_data = load_config (config_path);
    if (config_data == NULL) return EXIT_ERROR;

    // Apply CLI flags
    config_data->verbose = verbose_flag;
    config_data->json_output = json_flag;
    if (full_flag) {
        config_data->quick_check = FALSE;
    }

    if (config_data->verbose) {
        if (!g_setenv ("G_MESSAGES_DEBUG", "all", TRUE)) {
            g_log (NULL, G_LOG_LEVEL_WARNING, "Failed to set G_MESSAGES_DEBUG environment variable; continuing without verbose GLib messages");
        }
    }

    // Install logger now that config is loaded
    g_log_set_handler (NULL, G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION, log_handler, config_data);

    if (g_strcmp0 (command, "add") == 0) {
        config_data->mode = MODE_ADD;
    } else if (g_strcmp0 (command, "check") == 0) {
        config_data->mode = MODE_CHECK;
    } else if (g_strcmp0 (command, "update") == 0) {
        config_data->mode = MODE_UPDATE;
    } else if (g_strcmp0 (command, "stats") == 0) {
        config_data->mode = MODE_STATS;
    } else {
        show_help (argv[0]);
        free_config (config_data);
        return EXIT_ERROR;
    }

    DatabaseData *db_data = init_db (config_data);
    if (db_data == NULL) {
        free_config (config_data);
        return EXIT_ERROR;
    }

    // Handle stats command early - no scanning needed
    if (config_data->mode == MODE_STATS) {
        gboolean stats_ok = print_db_stats (db_data);
        free_db (db_data);
        free_config (config_data);
        return stats_ok ? EXIT_OK : EXIT_ERROR;
    }

    // Rolling scrub: with quick_check, the files whose path hash falls in the slice of the 64-bit
    // ring that became due since the last run are re-hashed in full. The slice is as long as the
    // time elapsed since the last run divided by scrub_days, and starts where the last one ended,
    // so every file is fully verified once per scrub_days regardless of how often (or rarely) we run.
    DbMeta meta;
    if (!db_meta_load (db_data, &meta)) {
        free_db (db_data);
        free_config (config_data);
        return EXIT_ERROR;
    }
    // One timestamp for the whole run: the slice covers the time up to the START of this run and the
    // next run measures from the same point, so the run's own duration is not lost (an HDD run can last hours).
    const gint64 run_start_us = g_get_real_time ();
    guint64 scrub_start = meta.scrub_cursor;
    guint64 scrub_len = 0;
    gboolean scrub_all = FALSE;
    if (config_data->quick_check && config_data->scrub_days > 0 &&
        (config_data->mode == MODE_CHECK || config_data->mode == MODE_UPDATE)) {
        gdouble period_us = (gdouble)config_data->scrub_days * 86400.0 * 1e6;
        gdouble elapsed_us = (meta.last_run_us > 0) ? (gdouble)MAX (run_start_us - meta.last_run_us, 0) : period_us;
        gdouble fraction = elapsed_us / period_us;
        if (fraction >= 1.0) {
            scrub_all = TRUE;
        } else {
            scrub_len = (guint64)(fraction * 18446744073709551616.0);
        }
    }

    // Start time and diagnostics
    GDateTime *start_wall = g_date_time_new_now_local ();
    gchar *start_ts = g_date_time_format (start_wall, "%Y-%m-%d %H:%M:%S %Z");
    gint64 start_mono_us = g_get_monotonic_time ();

    // Verbose/debug diagnostics
    g_debug ("Threads: %u (worker threads)", config_data->threads_count);
    g_debug ("Usable RAM: %" G_GUINT64_FORMAT " bytes", config_data->usable_ram);
    g_debug ("DB path: %s (size: %" G_GUINT64_FORMAT " bytes)", config_data->db_path, config_data->db_size_bytes);
    g_debug ("Directories: %s", config_data->directories);
    g_debug ("Max recursion depth: %u", config_data->max_recursion_depth);
    g_debug ("Exclude hidden: %s", config_data->exclude_hidden ? "yes" : "no");
    g_debug ("Quick check: %s", config_data->quick_check ? "yes" : "no");
    g_debug ("DB batch size: %u", config_data->db_batch_size);
    g_debug ("Scrub: %s", scrub_all ? "everything is due"
                         : scrub_len > 0 ? "rolling slice due"
                         : "nothing due yet");

    if (!config_data->json_output) {
        g_message ("Started %s at %s", config_data->mode == MODE_ADD ? "add" : config_data->mode == MODE_CHECK ? "check" : "update", start_ts);
    }

    gchar **dirs = parse_directories (config_data->directories);

    // HDD mode: ordered, prefetched reads. Auto-detect it from the devices behind the scanned directories.
    config_data->hdd_mode = (config_data->storage == STORAGE_HDD);
    if (config_data->storage == STORAGE_AUTO) {
        for (gsize d = 0; dirs[d] != NULL && !config_data->hdd_mode; d++) {
            if (storage_path_is_rotational (dirs[d])) {
                config_data->hdd_mode = TRUE;
                g_debug ("Rotational device detected for %s", dirs[d]);
            }
        }
    }
    if (config_data->low_priority) {
        set_idle_io_priority ();
    }
    g_debug ("Storage: %s mode", config_data->hdd_mode ? "HDD" : "SSD");

    FileQueueData *file_queue_data = init_file_queue (config_data->usable_ram, config_data->hdd_mode);
    if (!file_queue_data) {
        free_db (db_data);
        free_config (config_data);
        g_free (start_ts);
        g_date_time_unref (start_wall);
        return EXIT_ERROR;
    }
    g_atomic_int_set (&file_queue_data->scanning_done, FALSE);

    ConsumerData *consumer_data = g_try_new0 (ConsumerData, 1);
    if (!consumer_data) {
        free_db (db_data);
        free_config (config_data);
        g_critical ("Failed to allocate memory for consumer_data");
        g_free (start_ts);
        g_date_time_unref (start_wall);
        return EXIT_ERROR;
    }
    consumer_data->scrub_start = scrub_start;
    consumer_data->scrub_len = scrub_len;
    consumer_data->scrub_all = scrub_all;
    consumer_data->file_queue_data = file_queue_data;
    consumer_data->config_data = config_data;
    consumer_data->db_data = db_data;
    consumer_data->summary_data = summary_new ();
    if (consumer_data->summary_data == NULL) {
        free_db (db_data);
        free_config (config_data);
        g_free (start_ts);
        g_date_time_unref (start_wall);
        return EXIT_ERROR;
    }
    consumer_data->seen = seen_set_new ();
    if (consumer_data->seen == NULL) {
        g_critical ("Failed to allocate memory for the seen-paths set");
        free_summary (consumer_data->summary_data);
        free_db (db_data);
        free_config (config_data);
        g_free (start_ts);
        g_date_time_unref (start_wall);
        return EXIT_ERROR;
    }

    // Set up DB writer thread for write modes
    DbWriter *db_writer = NULL;
    if (config_data->mode == MODE_ADD || config_data->mode == MODE_UPDATE) {
        db_writer = db_writer_new (db_data, config_data->db_batch_size, consumer_data->summary_data,
                                   &file_queue_data->abort_requested);
        if (!db_writer) {
            seen_set_free (consumer_data->seen);
            free_summary (consumer_data->summary_data);
            free_db (db_data);
            free_config (config_data);
            g_free (start_ts);
            g_date_time_unref (start_wall);
            return EXIT_ERROR;
        }
    }
    consumer_data->db_writer = db_writer;

    // threads_count already honours io_threads (see load_config)
    GError *error = NULL;
    GThreadPool *thread_pool = g_thread_pool_new (worker_thread, consumer_data, (gint32)config_data->threads_count, FALSE, &error);
    if (error != NULL) {
        g_critical ("Error creating the thread pool: %s", error->message);
        g_error_free (error);
        if (db_writer) db_writer_destroy (db_writer);
        seen_set_free (consumer_data->seen);
        free_summary (consumer_data->summary_data);
        free_db (db_data);
        free_config (config_data);
        g_free (start_ts);
        g_date_time_unref (start_wall);
        return EXIT_ERROR;
    }
    consumer_data->thread_pool = thread_pool;

    consumer_data->sched = io_sched_new (consumer_data);
    if (consumer_data->sched == NULL) {
        g_thread_pool_free (thread_pool, TRUE, TRUE);
        if (db_writer) db_writer_destroy (db_writer);
        seen_set_free (consumer_data->seen);
        free_summary (consumer_data->summary_data);
        free_db (db_data);
        free_config (config_data);
        g_free (start_ts);
        g_date_time_unref (start_wall);
        return EXIT_ERROR;
    }

    GThread *consumer_thread = g_thread_new ("queue-consumer", queue_consumer, consumer_data);
    GThread *progress_thread = NULL;
    if (config_data->verbose && !config_data->json_output) {
        progress_thread = g_thread_new ("progress-reporter", progress_reporter, consumer_data);
    }

    GPtrArray *empty_roots = g_ptr_array_new_with_free_func (g_free);
    process_directories (dirs, config_data->max_recursion_depth, file_queue_data, config_data,
                         consumer_data->summary_data, empty_roots);
    g_strfreev (dirs);

    g_thread_join (consumer_thread);
    if (progress_thread) g_thread_join (progress_thread);
    g_thread_pool_free (thread_pool, FALSE, TRUE);
    io_sched_free (consumer_data->sched);

    // Flush and destroy DB writer before missing-files scan
    if (db_writer) {
        db_writer_destroy (db_writer);
        consumer_data->db_writer = NULL;
    }

    // After an abort the scan is incomplete; do not judge the rest of the database
    gboolean aborted = g_atomic_int_get (&file_queue_data->abort_requested);
    if (!aborted && config_data->mode == MODE_CHECK) {
        handle_missing_files_from_fs (db_data, consumer_data->summary_data, consumer_data->seen, empty_roots, FALSE);
    } else if (!aborted && config_data->mode == MODE_UPDATE) {
        handle_missing_files_from_fs (db_data, consumer_data->summary_data, consumer_data->seen, empty_roots, TRUE);
    }

    // Completed run: remember when, and where the next scrub slice starts. (Check mode writes
    // nothing else, but without this the scrub could never move forward.)
    if (!aborted) {
        meta.last_run_us = run_start_us;
        if (!scrub_all) meta.scrub_cursor = scrub_start + scrub_len;  // wraps around the ring
        if (!db_meta_store (db_data, &meta)) {
            summary_record_error (consumer_data->summary_data);
        }
    }

    // End time and duration
    GDateTime *end_wall = g_date_time_new_now_local ();
    gchar *end_ts = g_date_time_format (end_wall, "%Y-%m-%d %H:%M:%S %Z");
    gint64 end_mono_us = g_get_monotonic_time ();
    gdouble elapsed_sec = (end_mono_us - start_mono_us) / 1000000.0;

    if (!config_data->json_output) {
        g_message ("Completed at %s (duration: %.2f s)", end_ts, elapsed_sec);
    }

    if (config_data->json_output) {
        print_summary_json (consumer_data->summary_data, config_data->mode, elapsed_sec);
    } else {
        print_summary (consumer_data->summary_data, config_data->mode);
    }

    // Determine exit code
    // Errors win over "changes found": a run that could not read everything cannot vouch for it
    int exit_code = EXIT_OK;
    if (summary_get_errors (consumer_data->summary_data) > 0) {
        exit_code = EXIT_ERROR;
    } else if (config_data->mode == MODE_CHECK && consumer_data->summary_data->files_with_changes > 0) {
        exit_code = EXIT_CHANGES_FOUND;
    }

    g_ptr_array_free (empty_roots, TRUE);
    seen_set_free (consumer_data->seen);
    free_summary (consumer_data->summary_data);

    cleanup_logger ();

    g_free (start_ts);
    g_date_time_unref (start_wall);
    g_free (end_ts);
    g_date_time_unref (end_wall);

    free_file_queue (file_queue_data);
    free_config (config_data);
    free_db (db_data);

    g_free (consumer_data);

    return exit_code;
}
