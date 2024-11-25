#include <glib.h>
#include "config.h"
#include "database.h"
#include "process_directories.h"
#include "queue.h"
#include "process_file.h"
#include "version.h"
#include "logging.h"


void
show_help(const gchar *prog_name)
{
    g_print ("%s v%s\n", prog_name, VERSION);
    g_print ("Project URL: https://github.com/paolostivanin/FastFileCheck\n\n");
    g_print ("Usage:\n");
    g_print ("  %s [OPTION] COMMAND\n\n", prog_name);
    g_print ("Commands:\n");
    g_print ("  add     Add files to the database\n");
    g_print ("  check   Check files against the database\n");
    g_print ("  update  Remove/update files in the database\n\n");
    g_print ("Options:\n");
    g_print ("  -h, --help     Show this help message and exit\n");
    g_print ("  -v, --version  Show version information and exit\n");
    g_print ("  -c, --config   Path to config file (default: /etc/ffc.conf)\n");
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
        g_thread_pool_push (consumer_data->thread_pool, file_path, NULL);
    }
    return NULL;
}


int
main (int argc, char *argv[])
{
    if (argc > 1 && (g_strcmp0 (argv[1], "-h") == 0 || g_strcmp0 (argv[1], "--help") == 0)) {
        show_help (argv[0]);
        return 0;
    }

    const char *config_path = NULL;
    if (argc == 4 && strcmp (argv[1], "--config") == 0) {
        config_path = argv[2];
        argc -= 2;
        argv += 2;
    }

    if (argc != 2) {
        show_help (argv[0]);
        return -1;
    }

    ConfigData *config_data = load_config (config_path);
    if (config_data == NULL) return -1;

    if (strcmp (argv[1], "add") == 0) {
        config_data->mode = MODE_ADD;
    } else if (strcmp (argv[1], "check") == 0) {
        config_data->mode = MODE_CHECK;
    } else if (strcmp (argv[1], "update") == 0) {
        config_data->mode = MODE_UPDATE;
    } else {
        show_help (argv[0]);
        return -1;
    }

    g_log_set_default_handler (log_handler, NULL);

    DatabaseData *db_data = init_db (config_data);
    if (db_data == NULL) return -1;

    FileQueueData *file_queue_data = init_file_queue (config_data->usable_ram);
    file_queue_data->scanning_done = FALSE;

    ConsumerData *consumer_data = g_new0 (ConsumerData, 1);
    consumer_data->file_queue_data = file_queue_data;
    consumer_data->config_data = config_data;
    consumer_data->db_data = db_data;

    GError *error = NULL;
    GThreadPool *thread_pool = g_thread_pool_new (worker_thread, consumer_data, (gint32)config_data->threads_count, FALSE, &error);
    if (error != NULL) {
        gchar *msg = g_strconcat ("Error creating the thread pool: ", error->message, NULL);
        g_log (NULL, G_LOG_LEVEL_ERROR, msg);
        g_free (msg);
        g_error_free (error);
        return -1;
    }
    consumer_data->thread_pool = thread_pool;

    GThread *consumer_thread = g_thread_new ("queue-consumer", queue_consumer, consumer_data);

    gchar **dirs = g_strsplit (config_data->directories, ",", -1);
    process_directories (dirs, config_data->max_recursion_depth, file_queue_data, config_data);
    g_strfreev (dirs);

    g_thread_join (consumer_thread);
    g_thread_pool_free (thread_pool, FALSE, TRUE);

    free_file_queue (file_queue_data);
    free_config (config_data);
    free_db (db_data);

    g_free (consumer_data);

    return 0;
}