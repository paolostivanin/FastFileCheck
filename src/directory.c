#include <glib.h>
#include "directory.h"

#define MAX_RECURSION_DEPTH 10

typedef struct process_context_t {
    GHashTable *visited; // Tracks visited directories to prevent loops
    gint depth;          // Current recursion depth
} ProcessContext;


static void
scan_dir (const gchar    *dir_path,
          ProcessContext *ctx,
          FileQueueData  *file_queue_data)
{
    if (ctx->depth > MAX_RECURSION_DEPTH) {
        g_print ("Max recursion depth exceeded at: %s", dir_path);
        return;
    }

    // Check if the directory has been visited
    if (g_hash_table_contains(ctx->visited, dir_path)) {
        g_print ("Skipping already visited directory: %s", dir_path);
        return;
    }

    // Mark directory as visited
    g_hash_table_add(ctx->visited, g_strdup(dir_path));

    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir) {
        g_print ("Failed to open directory: %s", dir_path);
        return;
    }

    const gchar *entry;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        gchar *full_path = g_build_filename(dir_path, entry, NULL);

        if (g_file_test(full_path, G_FILE_TEST_IS_DIR)) {
            ctx->depth++;
            scan_dir (full_path, ctx, file_queue_data); // Recurse into subdirectory
            ctx->depth--;
        } else if (g_file_test(full_path, G_FILE_TEST_IS_REGULAR)) {
            while (g_async_queue_length (file_queue_data->queue) >= file_queue_data->max_size) {
                g_usleep (5000); // Sleep for 5 ms
            }
            g_async_queue_push (file_queue_data->queue, g_strdup (full_path));
        }
        g_free(full_path);
    }
    g_dir_close(dir);
}


void
process_directory (const gchar   *dir_path,
                   FileQueueData *file_queue_data)
{
    ProcessContext *ctx = g_new0 (ProcessContext, 1);
    ctx->visited = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    ctx->depth = 0;

    scan_dir (dir_path, ctx, file_queue_data);
    g_async_queue_push (file_queue_data->queue, NULL);

    file_queue_data->scanning_done = TRUE;

    g_hash_table_destroy (ctx->visited);
}
