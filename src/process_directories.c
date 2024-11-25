#include <glib.h>
#include <gio/gio.h>
#include "process_directories.h"

#define QUEUE_BUFFER_SIZE 1000
#define PATH_BUFFER_SIZE PATH_MAX

typedef struct {
    GHashTable *excluded_dirs;
    GHashTable *excluded_exts;
    gboolean exclude_hidden;
    GPtrArray *queue_buffer;
} ScanContext;

typedef struct process_context_t {
    GHashTable *visited; // Tracks visited directories to prevent loops
    guint depth;          // Current recursion depth
} ProcessContext;


static void
flush_queue_buffer (GPtrArray   *buffer,
                    GAsyncQueue *queue,
                    guint        max_size)
{
    for (guint i = 0; i < buffer->len; i++) {
        while (g_async_queue_length (queue) >= max_size) {
            g_usleep (5000);
        }
        g_async_queue_push (queue, buffer->pdata[i]);
    }
    g_ptr_array_set_size (buffer, 0);
}

static gboolean
should_skip_entry(const gchar *entry_name,
                  const gchar *full_path,
                  ScanContext *scan_ctx)
{
    if (scan_ctx->exclude_hidden && entry_name[0] == '.') {
        return TRUE;
    }

    if (scan_ctx->excluded_dirs && g_hash_table_contains (scan_ctx->excluded_dirs, full_path)) {
        return TRUE;
    }

    if (scan_ctx->excluded_exts) {
        const gchar *dot = strrchr (entry_name, '.');
        if (dot && g_hash_table_contains (scan_ctx->excluded_exts, dot)) {
            return TRUE;
        }
    }

    return FALSE;
}

static void
scan_dir(const gchar    *dir_path,
         guint           max_depth,
         ProcessContext *ctx,
         FileQueueData  *file_queue_data,
         ScanContext    *scan_ctx)
{
    if (ctx->depth > max_depth) {
        g_print ("Max recursion depth exceeded at: %s\n", dir_path);
        return;
    }

    if (g_hash_table_contains (ctx->visited, dir_path)) {
        return;
    }

    g_hash_table_add (ctx->visited, g_strdup (dir_path));

    GFile *dir = g_file_new_for_path (dir_path);
    GFileEnumerator *enumerator = g_file_enumerate_children (dir,
                                                             G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                                             G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                                             G_FILE_QUERY_INFO_NONE,
                                                             NULL,
                                                             NULL);

    if (!enumerator) {
        g_print ("Failed to open directory: %s\n", dir_path);
        g_object_unref (dir);
        return;
    }

    GFileInfo *info;
    gchar path_buffer[PATH_BUFFER_SIZE];

    while ((info = g_file_enumerator_next_file (enumerator, NULL, NULL))) {
        const gchar *entry = g_file_info_get_name (info);
        g_snprintf (path_buffer, PATH_BUFFER_SIZE, "%s/%s", dir_path, entry);

        if (!should_skip_entry (entry, path_buffer, scan_ctx)) {
            if (g_file_info_get_file_type (info) == G_FILE_TYPE_DIRECTORY) {
                ctx->depth++;
                scan_dir (path_buffer, max_depth, ctx, file_queue_data, scan_ctx);
                ctx->depth--;
            } else if (g_file_info_get_file_type (info) == G_FILE_TYPE_REGULAR) {
                g_ptr_array_add (scan_ctx->queue_buffer, g_strdup(path_buffer));

                if (scan_ctx->queue_buffer->len >= QUEUE_BUFFER_SIZE) {
                    flush_queue_buffer (scan_ctx->queue_buffer, file_queue_data->queue, file_queue_data->max_size);
                }
            }
        }
        g_object_unref (info);
    }

    g_object_unref (enumerator);
    g_object_unref (dir);
}

void
process_directories(gchar         **dirs,
                    guint           max_depth,
                    FileQueueData  *file_queue_data,
                    ConfigData     *config_data)
{
    ProcessContext *ctx = g_new0 (ProcessContext, 1);
    ctx->visited = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    ctx->depth = 0;

    ScanContext *scan_ctx = g_new0 (ScanContext, 1);
    scan_ctx->exclude_hidden = config_data->exclude_hidden;
    scan_ctx->queue_buffer = g_ptr_array_new ();

    if (config_data->exclude_directories) {
        scan_ctx->excluded_dirs = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        gchar **excluded = g_strsplit (config_data->exclude_directories, ";", -1);
        for (gsize i = 0; excluded[i]; i++) {
            g_hash_table_add (scan_ctx->excluded_dirs, g_strdup(excluded[i]));
        }
        g_strfreev (excluded);
    }

    if (config_data->exclude_extensions) {
        scan_ctx->excluded_exts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
        gchar **excluded = g_strsplit (config_data->exclude_extensions, ";", -1);
        for (gsize i = 0; excluded[i]; i++) {
            g_hash_table_add (scan_ctx->excluded_exts, g_strdup(excluded[i]));
        }
        g_strfreev (excluded);
    }

    for (gsize i = 0; dirs[i] != NULL; i++) {
        scan_dir (dirs[i], max_depth, ctx, file_queue_data, scan_ctx);
    }

    // Flush any remaining files in the buffer
    if (scan_ctx->queue_buffer->len > 0) {
        flush_queue_buffer (scan_ctx->queue_buffer, file_queue_data->queue, file_queue_data->max_size);
    }

    file_queue_data->scanning_done = TRUE;

    g_hash_table_destroy (ctx->visited);
    g_free (ctx);
    if (scan_ctx->excluded_dirs) g_hash_table_destroy (scan_ctx->excluded_dirs);
    if (scan_ctx->excluded_exts) g_hash_table_destroy (scan_ctx->excluded_exts);
    g_ptr_array_free (scan_ctx->queue_buffer, TRUE);
    g_free (scan_ctx);
}
