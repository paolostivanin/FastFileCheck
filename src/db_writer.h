#pragma once

#include <glib.h>
#include <lmdb.h>
#include "config.h"
#include "database.h"
#include "summary.h"

typedef struct db_write_entry_t {
    gchar *filepath;
    void *entry_data;
    gsize entry_size;
} DbWriteEntry;

typedef struct db_writer_t {
    GAsyncQueue *write_queue;
    DatabaseData *db_data;
    SummaryData *summary_data;  // lost entries are counted as errors here
    gint *abort_flag;           // set to TRUE (atomically) when the DB is full: the run cannot succeed
    guint batch_size;
    GThread *thread;
    gint done;                  // atomic: no more entries will be pushed
    gboolean map_full;          // writer thread only
} DbWriter;

DbWriter *db_writer_new     (DatabaseData *db_data,
                             guint         batch_size,
                             SummaryData  *summary_data,
                             gint         *abort_flag);

void      db_writer_push    (DbWriter    *writer,
                             const gchar *filepath,
                             const void  *entry_data,
                             gsize        entry_size);

// Stop accepting entries, commit what is pending and join the thread. Called by destroy.
void      db_writer_flush   (DbWriter    *writer);

void      db_writer_destroy (DbWriter    *writer);
