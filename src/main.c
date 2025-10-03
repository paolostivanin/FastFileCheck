#include <glib.h>
#include "config.h"
#include "database.h"
#include "process_directories.h"
#include "queue.h"
#include "process_file.h"
#include "version.h"
#include "logging.h"
#include "summary.h"


void
show_help(const gchar *prog_name)
{
    g_print ("Project URL: https://github.com/paolostivanin/FastFileCheck\n\n");
    g_print ("Usage:\n");
    g_print ("  %s [OPTIONS] COMMAND\n\n", prog_name);
    g_print ("Commands:\n");
    g_print ("  add     Add files to the database\n");
    g_print ("  check   Check files against the database\n");
    g_print ("  update  Remove/update files in the database\n\n");
    g_print ("Options:\n");
    g_print ("  -h, --help      Show this help message and exit\n");
    g_print ("  -v, --version   Show version information and exit\n");
    g_print ("  -c, --config    Path to config file (default: /etc/ffc.conf)\n");
    g_print ("  -V, --verbose   Verbose output with heartbeat/progress\n");
}


static void
worker_thread (gpointer data,
               gpointer user_data)
{
    gchar *file_path = (gchar *)data;
    ConsumerData *consumer_data = (ConsumerData *)user_data;
    process_file (file_path, consumer_data);
}


static gpointer
queue_consumer(gpointer data)
{
    ConsumerData *consumer_data = (ConsumerData *)data;
    while (TRUE) {
        gchar *file_path = g_async_queue_try_pop (consumer_data->file_queue_data->queue);
        if (file_path == NULL) {
            if (consumer_data->file_queue_data->scanning_done) {
                // Drain any remaining items
                while ((file_path = g_async_queue_try_pop (consumer_data->file_queue_data->queue)) != NULL) {
                    g_thread_pool_push (consumer_data->thread_pool, file_path, NULL);
                }
                break;
            }
            g_usleep (1000);
        }
        if (file_path != NULL) g_thread_pool_push (consumer_data->thread_pool, file_path, NULL);
    }
    return NULL;
}

static gpointer
progress_reporter (gpointer data)
{
    ConsumerData *consumer_data = (ConsumerData *)data;
    // Periodically report progress until work is done
    while (TRUE) {
        g_usleep (2 * 1000 * 1000);
        guint qlen = (guint)g_async_queue_length (consumer_data->file_queue_data->queue);
        gboolean done = consumer_data->file_queue_data->scanning_done;
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
    // Basic option parsing: allow --help/--version/--config PATH/--verbose before COMMAND
    const char *config_path = NULL;
    gboolean verbose_flag = FALSE;

    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (g_strcmp0 (argv[i], "-h") == 0 || g_strcmp0 (argv[i], "--help") == 0) {
            show_help (argv[0]);
            return 0;
        } else if (g_strcmp0 (argv[i], "-v") == 0 || g_strcmp0 (argv[i], "--version") == 0) {
            g_print ("%s v%s\n", argv[0], FASTFILECHECK_VERSION_FULL);
            return 0;
        } else if (g_strcmp0 (argv[i], "-c") == 0 || g_strcmp0 (argv[i], "--config") == 0) {
            if (i + 1 >= argc) {
                show_help (argv[0]);
                return -1;
            }
            config_path = argv[i + 1];
            i += 2;
            continue;
        } else if (g_strcmp0 (argv[i], "-V") == 0 || g_strcmp0 (argv[i], "--verbose") == 0) {
            verbose_flag = TRUE;
            i++;
            continue;
        } else {
            break;
        }
    }

    if (i >= argc) {
        show_help (argv[0]);
        return -1;
    }

    const char *command = argv[i];

    ConfigData *config_data = load_config (config_path);
    if (config_data == NULL) return -1;

    // Apply CLI verbose preference
    config_data->verbose = verbose_flag;
    if (config_data->verbose) {
        if (!g_setenv ("G_MESSAGES_DEBUG", "all", TRUE)) {
            g_log (NULL, G_LOG_LEVEL_WARNING, "Failed to set G_MESSAGES_DEBUG environment variable; continuing without verbose GLib messages");
        }
    }

    // Install logger now that config is loaded
    g_log_set_handler (NULL, G_LOG_LEVEL_MASK | G_LOG_FLAG_FATAL | G_LOG_FLAG_RECURSION, log_handler, config_data);

    // Start time and diagnostics
    GDateTime *start_wall = g_date_time_new_now_local ();
    gchar *start_ts = g_date_time_format (start_wall, "%Y-%m-%d %H:%M:%S %Z");
    gint64 start_mono_us = g_get_monotonic_time ();

    // Verbose/debug diagnostics
    g_debug ("Threads: %u (worker threads)", config_data->threads_count);
    g_debug ("Usable RAM: %" G_GUINT64_FORMAT " bytes", config_data->usable_ram);
    g_debug ("Max RAM per thread: %" G_GUINT64_FORMAT " bytes", config_data->max_ram_per_thread);
    g_debug ("DB path: %s (size: %u bytes)", config_data->db_path, config_data->db_size_bytes);
    g_debug ("Directories: %s", config_data->directories);
    g_debug ("Max recursion depth: %u", config_data->max_recursion_depth);
    g_debug ("Exclude hidden: %s", config_data->exclude_hidden ? "yes" : "no");

    if (g_strcmp0 (command, "add") == 0) {
        config_data->mode = MODE_ADD;
    } else if (g_strcmp0 (command, "check") == 0) {
        config_data->mode = MODE_CHECK;
    } else if (g_strcmp0 (command, "update") == 0) {
        config_data->mode = MODE_UPDATE;
    } else {
        show_help (argv[0]);
        return -1;
    }

    g_message ("Started %s at %s", config_data->mode == MODE_ADD ? "add" : config_data->mode == MODE_CHECK ? "check" : "update", start_ts);

    DatabaseData *db_data = init_db (config_data);
    if (db_data == NULL) return -1;

    FileQueueData *file_queue_data = init_file_queue (config_data->usable_ram);
    if (!file_queue_data) {
        free_db (db_data);
        free_config (config_data);
        return -1;
    }
    file_queue_data->scanning_done = FALSE;

    ConsumerData *consumer_data = g_try_new0 (ConsumerData, 1);
    if (!consumer_data) {
        free_db (db_data);
        free_config (config_data);
        g_log (NULL, G_LOG_LEVEL_ERROR, "Failed to allocate memory for consumer_data");
        return -1;
    }
    consumer_data->file_queue_data = file_queue_data;
    consumer_data->config_data = config_data;
    consumer_data->db_data = db_data;
    consumer_data->summary_data = summary_new ();
    if (consumer_data->summary_data == NULL) {
        free_db (db_data);
        free_config (config_data);
        return -1;
    }

    GError *error = NULL;
    GThreadPool *thread_pool = g_thread_pool_new (worker_thread, consumer_data, (gint32)config_data->threads_count, FALSE, &error);
    if (error != NULL) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Error creating the thread pool: %s", error->message);
        g_error_free (error);
        return -1;
    }
    consumer_data->thread_pool = thread_pool;

    GThread *consumer_thread = g_thread_new ("queue-consumer", queue_consumer, consumer_data);
    GThread *progress_thread = NULL;
    if (config_data->verbose) {
        progress_thread = g_thread_new ("progress-reporter", progress_reporter, consumer_data);
    }

    gchar **dirs = g_strsplit (config_data->directories, ",", -1);
    process_directories (dirs, config_data->max_recursion_depth, file_queue_data, config_data);
    g_strfreev (dirs);

    g_thread_join (consumer_thread);
    if (progress_thread) g_thread_join (progress_thread);
    g_thread_pool_free (thread_pool, FALSE, TRUE);

    if (config_data->mode == MODE_CHECK) {
        handle_missing_files_from_fs (db_data, consumer_data->summary_data, FALSE);
    } else if (config_data->mode == MODE_UPDATE) {
        handle_missing_files_from_fs (db_data, consumer_data->summary_data, TRUE);
    }

    // End time and duration
    GDateTime *end_wall = g_date_time_new_now_local ();
    gchar *end_ts = g_date_time_format (end_wall, "%Y-%m-%d %H:%M:%S %Z");
    gint64 end_mono_us = g_get_monotonic_time ();
    gdouble elapsed_sec = (end_mono_us - start_mono_us) / 1000000.0;
    g_message ("Completed at %s (duration: %.2f s)", end_ts, elapsed_sec);

    print_summary (consumer_data->summary_data, config_data->mode);
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

    return 0;
}
