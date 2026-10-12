#include <glib.h>
#include <lmdb.h>
#include <string.h>
#include "db_writer.h"

#define WRITER_POP_TIMEOUT_US     (100 * 1000)        // how often the idle writer wakes up
#define PARTIAL_FLUSH_AGE_US      (1000 * 1000)       // commit a partial batch once it is this old
#define MIN_QUEUE_LIMIT           4096                // backpressure: max entries waiting for the writer
#define QUEUE_LIMIT_BATCH_FACTOR  8


static void
free_write_entry (DbWriteEntry *entry)
{
    if (entry) {
        g_free (entry->filepath);
        g_free (entry->entry_data);
        g_free (entry);
    }
}


static gint
compare_entries (gconstpointer a,
                 gconstpointer b)
{
    const DbWriteEntry *ea = *(DbWriteEntry * const *)a;
    const DbWriteEntry *eb = *(DbWriteEntry * const *)b;
    return strcmp (ea->filepath, eb->filepath);
}


static void
fail_batch (DbWriter   *writer,
            guint       lost,
            const char *what,
            int         rc)
{
    if (rc == MDB_MAP_FULL) {
        if (!writer->map_full) {
            writer->map_full = TRUE;
            g_critical ("The database is full: increase db_size_mb in the config file and run again. Stopping.");
            if (writer->abort_flag) g_atomic_int_set (writer->abort_flag, TRUE);
        }
    } else {
        g_critical ("db_writer: %s failed: %s (%u entries not written)", what, mdb_strerror (rc), lost);
    }

    for (guint i = 0; i < lost; i++) {
        summary_record_error (writer->summary_data);
    }
}


static void
commit_batch (DbWriter  *writer,
              GPtrArray *batch)
{
    if (batch->len == 0) return;

    if (writer->map_full) {
        // Already reported; drop the entries and keep the error count honest
        fail_batch (writer, batch->len, "commit", MDB_MAP_FULL);
        return;
    }

    // Sorted keys touch the B-tree leaves in order instead of at random
    g_ptr_array_sort (batch, compare_entries);

    MDB_txn *txn;
    int rc = mdb_txn_begin (writer->db_data->env, NULL, 0, &txn);
    if (rc != 0) {
        fail_batch (writer, batch->len, "mdb_txn_begin", rc);
        return;
    }

    for (guint i = 0; i < batch->len; i++) {
        DbWriteEntry *entry = g_ptr_array_index (batch, i);
        MDB_val key = { .mv_size = strlen (entry->filepath), .mv_data = entry->filepath };
        MDB_val data = { .mv_size = entry->entry_size, .mv_data = entry->entry_data };

        rc = mdb_put (txn, writer->db_data->dbi, &key, &data, 0);
        if (rc == MDB_BAD_VALSIZE) {
            // Rejected before anything was modified, so the transaction is still good: lose this
            // entry only (the workers already screen path lengths, this is the safety net).
            g_critical ("db_writer: key or value size not supported by LMDB, skipping %s", entry->filepath);
            summary_record_error (writer->summary_data);
            continue;
        }
        if (rc != 0) {
            mdb_txn_abort (txn);
            fail_batch (writer, batch->len, "mdb_put", rc);
            return;
        }
    }

    rc = mdb_txn_commit (txn);
    if (rc != 0) {
        fail_batch (writer, batch->len, "mdb_txn_commit", rc);
    }
}


static gpointer
writer_thread_func (gpointer data)
{
    DbWriter *writer = (DbWriter *)data;
    GPtrArray *batch = g_ptr_array_new_with_free_func ((GDestroyNotify)free_write_entry);
    gint64 batch_started_us = 0;

    while (TRUE) {
        DbWriteEntry *entry = g_async_queue_timeout_pop (writer->write_queue, WRITER_POP_TIMEOUT_US);

        if (entry) {
            if (batch->len == 0) batch_started_us = g_get_monotonic_time ();
            g_ptr_array_add (batch, entry);

            if (batch->len >= writer->batch_size) {
                commit_batch (writer, batch);
                g_ptr_array_set_size (batch, 0);
            }
        } else if (g_atomic_int_get (&writer->done) && g_async_queue_length (writer->write_queue) == 0) {
            commit_batch (writer, batch);
            g_ptr_array_set_size (batch, 0);
            break;
        }

        // Never leave a partial batch sitting around for long, but do not commit on every
        // short idle gap either: on a slow disk each commit is an fsync, and files arrive
        // slower than a few ms apart.
        if (batch->len > 0 && g_get_monotonic_time () - batch_started_us >= PARTIAL_FLUSH_AGE_US) {
            commit_batch (writer, batch);
            g_ptr_array_set_size (batch, 0);
        }
    }

    g_ptr_array_free (batch, TRUE);
    return NULL;
}


DbWriter *
db_writer_new (DatabaseData *db_data,
               guint         batch_size,
               SummaryData  *summary_data,
               gint         *abort_flag)
{
    DbWriter *writer = g_try_new0 (DbWriter, 1);
    if (!writer) {
        g_critical ("Failed to allocate memory for DbWriter");
        return NULL;
    }

    writer->db_data = db_data;
    writer->summary_data = summary_data;
    writer->abort_flag = abort_flag;
    writer->batch_size = (batch_size > 0) ? batch_size : 500;
    writer->write_queue = g_async_queue_new_full ((GDestroyNotify)free_write_entry);
    writer->thread = g_thread_new ("db-writer", writer_thread_func, writer);

    return writer;
}


void
db_writer_push (DbWriter    *writer,
                const gchar *filepath,
                const void  *entry_data,
                gsize        entry_size)
{
    // Backpressure: if the disk cannot keep up with commits, slow the producers down
    // instead of buffering the whole run in memory.
    gint limit = (gint)MAX (writer->batch_size * QUEUE_LIMIT_BATCH_FACTOR, (guint)MIN_QUEUE_LIMIT);
    while (g_async_queue_length (writer->write_queue) > limit) {
        g_usleep (1000);
    }

    DbWriteEntry *entry = g_new (DbWriteEntry, 1);
    entry->filepath = g_strdup (filepath);
    entry->entry_data = g_memdup2 (entry_data, entry_size);
    entry->entry_size = entry_size;
    g_async_queue_push (writer->write_queue, entry);
}


void
db_writer_flush (DbWriter *writer)
{
    g_atomic_int_set (&writer->done, TRUE);
    if (writer->thread) {
        g_thread_join (writer->thread);
        writer->thread = NULL;
    }
}


void
db_writer_destroy (DbWriter *writer)
{
    if (!writer) return;
    if (writer->thread) {
        db_writer_flush (writer);
    }
    g_async_queue_unref (writer->write_queue);
    g_free (writer);
}
