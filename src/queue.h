#pragma once

#include <glib.h>

typedef struct file_queue_t {
    GAsyncQueue *queue;
    gint max_size;
    gboolean scanning_done;
} FileQueueData;

FileQueueData *init_file_queue (guint64 usable_ram);

void free_file_queue (FileQueueData *file_queue_data);