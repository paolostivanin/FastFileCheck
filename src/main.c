#include <glib.h>
#include "config.h"
#include "database.h"
#include "directory.h"
#include "queue.h"

static void
usage (const char *prog_name)
{
    g_print ("Usage: %s [--config /path/to/cfg] <add|check> <directory_path>\n", prog_name);
}


void
worker_thread (gpointer data, gpointer user_data)
{
//    while (true) {
//        Task task = dequeue_task ();
//
//        if (task.filepath == NULL) break;
//
//        g_print ("Processing file: %s\n", task.filepath);
//        //process_file (task.filepath);
//        free (task.filepath);
//    }
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
    config_data->mode = (strcmp (argv[1], "add") == 0) ? MODE_ADD : MODE_CHECK;

    DatabaseData *db_data = init_db (config_data);
    if (db_data == NULL) return -1;

    GError *error = NULL;
    GThreadPool *thread_pool = g_thread_pool_new (worker_thread, NULL, (gint32)config_data->threads_count, FALSE, &error);
    if (error != NULL) {
        g_printerr ("Error creating the thread pool: %s\n", error->message);
        g_error_free (error);
        return -1;
    }

    FileQueueData *file_queue_data = init_file_queue (config_data->usable_ram);
    process_directory (argv[2], file_queue_data);

    g_thread_pool_free (thread_pool, FALSE, TRUE);

    free_file_queue (file_queue_data);

    mdb_env_close (db_data->env);

    free_config (config_data);

    return 0;
}