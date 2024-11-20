#include <glib.h>
#include <gio/gio.h>
#include <xxhash.h>
#include "queue.h"

#define MMAP_THRESHOLD_RATIO 0.75

static guint64
compute_hash (const char *filepath,
              guint64 per_thread_ram)
{
    GFile *file = g_file_new_for_path (filepath);
    GFileInfo *file_info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
    if (!file_info) {
        g_object_unref(file);
        return 0;
    }
    goffset file_size = g_file_info_get_size (file_info);
    g_object_unref (file_info);

    // Use memory mapping if file size is less than 75% of per-thread RAM
    if (file_size > 0 && (gdouble)file_size < ((gdouble)per_thread_ram * MMAP_THRESHOLD_RATIO)) {
        GMappedFile *mapped = g_mapped_file_new (filepath, FALSE, NULL);
        if (mapped) {
            gsize length;
            gchar *contents = g_mapped_file_get_contents (mapped);
            length = g_mapped_file_get_length (mapped);

            XXH64_hash_t hash = XXH3_64bits (contents, length);

            g_mapped_file_unref (mapped);
            g_object_unref (file);
            return hash;
        }
    }

    // Fall back to chunked reading
    GFileInputStream *input_stream = g_file_read (file, NULL, NULL);
    if (!input_stream) {
        g_object_unref (file);
        return 0;
    }

    // Calculate optimal buffer size:
    // - 25% of per-thread RAM
    // - Maximum 128 MB per thread
    // - Minimum  10 MB to ensure decent performance
    const gsize buffer_size = CLAMP (per_thread_ram / 4,
                                     10 * 1024 * 1024,
                                    128 * 1024 * 1024);

    guchar *buffer = g_malloc(buffer_size);

    XXH3_state_t *state = XXH3_createState();
    XXH3_64bits_reset(state);

    gssize bytes_read;
    while ((bytes_read = g_input_stream_read(G_INPUT_STREAM(input_stream),
                                             buffer, buffer_size, NULL, NULL)) > 0) {
        XXH3_64bits_update(state, buffer, bytes_read);
    }

    XXH64_hash_t hash = 0;
    if (bytes_read >= 0) {
        hash = XXH3_64bits_digest(state);
    }

    g_free(buffer);
    XXH3_freeState(state);
    g_object_unref(input_stream);
    g_object_unref(file);

    return hash;
}


void
process_file (const gchar  *file_path,
              ConsumerData *consumer_data)
{
    // TODO: Implement file processing logic
}