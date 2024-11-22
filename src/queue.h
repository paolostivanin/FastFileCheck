#pragma once

#include <glib.h>
#include "config.h"
#include "database.h"

typedef struct file_queue_t {
    GAsyncQueue *queue;
    gint max_size;
    gboolean scanning_done;
} FileQueueData;

typedef struct consumer_data_t {
    GThreadPool *thread_pool;
    FileQueueData *file_queue_data;
    ConfigData *config_data;
    DatabaseData *db_data;
} ConsumerData;

FileQueueData *init_file_queue (guint64        usable_ram);

void free_file_queue           (FileQueueData *file_queue_data);