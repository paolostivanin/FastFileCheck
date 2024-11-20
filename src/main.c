#include <glib.h>
#include "config.h"
#include "database.h"
#include "directory.h"
#include "queue.h"

typedef struct consumer_data_t {
    GThreadPool *thread_pool;
    FileQueueData *file_queue_data;
} ConsumerData;

static void
usage (const char *prog_name)
{
    g_print ("Usage: %s [--config /path/to/cfg] <add|check|update> <directory_path>\n", prog_name);
    // TODO: update will remove files that are no longer on the disk and also stuff like inode, hash, etc if changed
}


static void
worker_thread (gpointer data,
               gpointer user_data)
{
    gchar *file_path = (gchar *)data;
    FileQueueData *file_queue_data = (FileQueueData *)user_data;
    // TODO: Process the file here
}


static gpointer
queue_consumer (gpointer data)
{
    ConsumerData *consumer_data = (ConsumerData *)data;
    while (TRUE) {
        gchar *file_path = g_async_queue_try_pop (consumer_data->file_queue_data->queue);
        if (file_path == NULL) {
            if (consumer_data->file_queue_data->scanning_done) break;
            g_usleep (1000);
            continue;
        } else {
            g_thread_pool_push (consumer_data->thread_pool, file_path, NULL);
        }
    }
    return NULL;
}


int
main (int argc, char *argv[])
{
    const char *config_path = NULL;
    if (argc == 5 && strcmp (argv[1], "--config") == 0) {
        config_path = argv[2];
        argc -= 2;
        argv += 2;
    }

    if (argc != 3) {
        usage(argv[0]);
        return -1;
    }

    ConfigData *config_data = load_config (config_path);
    if (strcmp (argv[1], "add") == 0) {
        config_data->mode = MODE_ADD;
    } else if (strcmp (argv[1], "check") == 0) {
        config_data->mode = MODE_CHECK;
    } else if (strcmp (argv[1], "update") == 0) {
        config_data->mode = MODE_UPDATE;
    } else {
        usage(argv[0]);
        return -1;
    }

    DatabaseData *db_data = init_db (config_data);
    if (db_data == NULL) return -1;

    FileQueueData *file_queue_data = init_file_queue (config_data->usable_ram);
    file_queue_data->scanning_done = FALSE;

    GError *error = NULL;
    GThreadPool *thread_pool = g_thread_pool_new (worker_thread, file_queue_data, (gint32)config_data->threads_count, FALSE, &error);
    if (error != NULL) {
        g_printerr ("Error creating the thread pool: %s\n", error->message);
        g_error_free (error);
        return -1;
    }

    ConsumerData *consumer_data = g_new0 (ConsumerData, 1);
    consumer_data->thread_pool = thread_pool;
    consumer_data->file_queue_data = file_queue_data;
    GThread *consumer_thread = g_thread_new ("queue-consumer", queue_consumer, consumer_data);

    process_directory (argv[2], file_queue_data);

    g_thread_join (consumer_thread);
    g_thread_pool_free (thread_pool, FALSE, TRUE);

    free_file_queue (file_queue_data);

    mdb_env_close (db_data->env);

    free_config (config_data);

    g_free (consumer_data);

    return 0;
}