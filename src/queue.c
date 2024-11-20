#include <glib.h>
#include "queue.h"

#define AVERAGE_PATH_LENGTH 256  // Conservative estimate for path length
#define MEMORY_FACTOR        10  // Use 10% of available RAM for queue

static gint
get_max_queue_size (guint64 usable_ram)
{
    guint64 calculated_size = (usable_ram / MEMORY_FACTOR) / AVERAGE_PATH_LENGTH;

    // Cap at maximum gint value if needed
    if (calculated_size > G_MAXINT) {
        return G_MAXINT;
    }

    return (gint)calculated_size;
}


void
free_file_queue (FileQueueData *file_queue_data)
{
    g_async_queue_unref (file_queue_data->queue);
    g_free (file_queue_data);
}


FileQueueData *
init_file_queue (guint64 usable_ram)
{
    FileQueueData *file_queue_data = g_new0 (FileQueueData, 1);
    file_queue_data->queue = g_async_queue_new_full (g_free);
    file_queue_data->max_size = get_max_queue_size (usable_ram);
    return file_queue_data;
}