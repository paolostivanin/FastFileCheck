#pragma once

#include <glib.h>
#include "config.h"
#include "database.h"
#include "summary.h"
#include "seen_set.h"

typedef struct db_writer_t DbWriter;
typedef struct io_sched_t IoSched;

typedef struct file_queue_t {
    GAsyncQueue *queue;         // of FileTask *
    gint max_size;
    gint scanning_done;         // atomic: the scanner has queued every file
    gint abort_requested;       // atomic: set when the run cannot succeed (e.g. DB full); everybody stops early
} FileQueueData;

typedef struct consumer_data_t {
    GThreadPool *thread_pool;
    FileQueueData *file_queue_data;
    ConfigData *config_data;
    DatabaseData *db_data;
    SummaryData *summary_data;
    DbWriter *db_writer;
    SeenSet *seen;              // paths looked at by the workers (feeds the missing-file pass)
    IoSched *sched;             // decides in which order files reach the workers (HDD mode)

    // Rolling scrub: files whose path hash lies in [scrub_start, scrub_start + scrub_len) on the
    // 64-bit ring are re-hashed even if quick_check says they are unchanged (see scrub_selected)
    guint64 scrub_start;
    guint64 scrub_len;
    gboolean scrub_all;
} ConsumerData;

FileQueueData *init_file_queue (guint64        usable_ram,
                                gboolean       hdd_mode);

void free_file_queue           (FileQueueData *file_queue_data);
