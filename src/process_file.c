#include <glib.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <xxhash.h>
#include "queue.h"
#include "summary.h"

#define MMAP_THRESHOLD_RATIO 0.75
#define MIN_BUFFER_SIZE (10 * 1024 * 1024)  // 10MB
#define MAX_BUFFER_SIZE (128 * 1024 * 1024) // 128MB

typedef struct file_entry_t {
    gchar *filepath;
    guint64 hash;
    ino_t inode;
    nlink_t link_count;
    blkcnt_t block_count;
} FileEntryData;

typedef struct file_info_t {
    struct stat st;
    guint64 hash;
} FileInfo;


static gboolean
validate_filepath (const char *filepath)
{
    return filepath && *filepath && g_file_test(filepath, G_FILE_TEST_EXISTS);
}


static guint64
compute_hash (const char    *filepath,
              const guint64  per_thread_ram)
{
    GError *error = NULL;
    GFile *file = g_file_new_for_path (filepath);
    GFileInfo *file_info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, NULL, &error);
    if (!file_info) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Failed to query file info: %s\n", error->message);
        g_clear_error (&error);
        g_object_unref (file);
        return 0;
    }

    const goffset file_size = g_file_info_get_size (file_info);
    g_object_unref (file_info);

    // Use memory mapping if file size is less than 75% of per-thread RAM
    if (file_size > 0 && (gdouble)file_size < ((gdouble)per_thread_ram * MMAP_THRESHOLD_RATIO)) {
        GMappedFile *mapped = g_mapped_file_new (filepath, FALSE, NULL);
        if (mapped) {
            const gchar *contents = g_mapped_file_get_contents (mapped);
            gsize length = g_mapped_file_get_length (mapped);
            XXH64_hash_t hash = XXH3_64bits (contents, length);
            g_mapped_file_unref (mapped);
            g_object_unref (file);
            return hash;
        }
    }

    // Fall back to chunked reading
    g_log (NULL, G_LOG_LEVEL_INFO, "Falling back to chunked reading for file %s\n", filepath);
    GFileInputStream *input_stream = g_file_read (file, NULL, &error);
    if (!input_stream) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Failed to open file (%s) for reading: %s\n", filepath, error->message);
        g_clear_error (&error);
        g_object_unref (file);
        return 0;
    }

    const gsize buffer_size = CLAMP(per_thread_ram / 4, MIN_BUFFER_SIZE, MAX_BUFFER_SIZE);

    guchar *buffer = g_try_malloc0_n (buffer_size, sizeof(guchar));
    if (!buffer) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Failed to allocate buffer for file %s\n", filepath);
        g_object_unref (input_stream);
        g_object_unref (file);
        return 0;
    }

    XXH3_state_t *state = XXH3_createState ();
    if (!state) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Failed to create XXH3 state for file %s\n", filepath);
        g_free (buffer);
        g_object_unref (input_stream);
        g_object_unref (file);
        return 0;
    }

    XXH3_64bits_reset (state);

    gssize bytes_read;
    while ((bytes_read = g_input_stream_read (G_INPUT_STREAM(input_stream), buffer, buffer_size, NULL, NULL)) > 0) {
        XXH3_64bits_update (state, buffer, bytes_read);
    }

    XXH64_hash_t hash = 0;
    if (bytes_read >= 0) {
        hash = XXH3_64bits_digest (state);
    }

    g_free (buffer);
    XXH3_freeState (state);
    g_object_unref (input_stream);
    g_object_unref (file);
    g_clear_error (&error);

    return hash;
}


static gboolean
get_file_info (const char    *filepath,
               const guint64  per_thread_ram,
               FileInfo      *info)
{
    if (stat (filepath, &info->st) != 0) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Could not stat file: %s\n", filepath);
        return FALSE;
    }

    info->hash = compute_hash (filepath, per_thread_ram);
    if (info->hash == 0) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Could not compute hash for file: %s\n", filepath);
        return FALSE;
    }

    return TRUE;
}

static FileEntryData
create_entry_data (const char     *filepath,
                   const FileInfo *info)
{
    return (FileEntryData) {
        .filepath = g_strdup (filepath),
        .hash = info->hash,
        .inode = info->st.st_ino,
        .link_count = info->st.st_nlink,
        .block_count = info->st.st_blocks
    };
}

static gboolean
handle_db_operation (const char     *filepath,
                     const FileInfo *info,
                     DatabaseData   *db_data,
                     SummaryData    *summary_data,
                     Mode            op)
{
    MDB_txn *txn;
    MDB_val key, data;
    int flags = (op == MODE_CHECK) ? MDB_RDONLY : 0;

    int rc = mdb_txn_begin (db_data->env, NULL, flags, &txn);
    if (rc != 0) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "mdb_txn_begin failed: %s\n", mdb_strerror (rc));
        return FALSE;
    }

    // LMDB expects key size in bytes, not UTF-8 character count
    key.mv_size = strlen (filepath) + 1;
    key.mv_data = (void*)filepath;
    // TODO: how to add verbosity? Currently nothing is shown (log file? print? what?)
    if (op == MODE_ADD) {
        FileEntryData entry = create_entry_data (filepath, info);
        data.mv_size = sizeof(FileEntryData);
        data.mv_data = &entry;

        rc = mdb_put (txn, db_data->dbi, &key, &data, 0);
        if (rc != 0) {
            g_log (NULL, G_LOG_LEVEL_ERROR, "mdb_put failed: %s\n", mdb_strerror (rc));
            mdb_txn_abort (txn);
            g_free (entry.filepath);
            return FALSE;
        }
        g_free(entry.filepath);
        summary_data->total_files_processed++;
    } else {
        rc = mdb_get (txn, db_data->dbi, &key, &data);
        if (rc != 0) {
            if (rc != MDB_NOTFOUND) {
                // The only error we expect is MDB_NOTFOUND, which means the file is not in the database (e.g. created after add operation)
                g_log (NULL, G_LOG_LEVEL_ERROR, "Database operation failed: %s\n", mdb_strerror (rc));
                mdb_txn_abort (txn);
                return FALSE;
            }
            // File not found, so in UPDATE mode we add it to the dabase, while in CHECK mode we record it as missing
            if (op == MODE_UPDATE) {
                // For update operation, add if not found
                FileEntryData entry = create_entry_data (filepath, info);
                data.mv_size = sizeof(FileEntryData);
                data.mv_data = &entry;

                rc = mdb_put (txn, db_data->dbi, &key, &data, 0);
                if (rc != 0) {
                    g_log (NULL, G_LOG_LEVEL_ERROR, "mdb_put failed: %s\n", mdb_strerror (rc));
                    mdb_txn_abort (txn);
                    g_free (entry.filepath);
                    return FALSE;
                }
                g_free (entry.filepath);
            }
            if (op == MODE_CHECK) {
                record_change (summary_data, filepath, CHANGE_MISSING_IN_DB);
            }
            summary_data->total_files_processed++;
        } else {
            FileEntryData *stored = (FileEntryData *)data.mv_data;
            if (op == MODE_CHECK) {
                gboolean change_recorded = FALSE;
                if (info->hash != stored->hash) {
                    record_change (summary_data, filepath, CHANGE_HASH);
                    change_recorded = TRUE;
                }
                if (info->st.st_ino != stored->inode) {
                    record_change (summary_data, filepath, CHANGE_INODE);
                    change_recorded = TRUE;
                }
                if (info->st.st_nlink != stored->link_count) {
                    record_change (summary_data, filepath, CHANGE_LINKS);
                    change_recorded = TRUE;
                }
                if (info->st.st_blocks != stored->block_count) {
                    record_change (summary_data, filepath, CHANGE_BLOCKS);
                    change_recorded = TRUE;
                }
                if (!change_recorded) {
                    summary_data->total_files_processed++;
                }
            } else if (op == MODE_UPDATE &&
                      (info->hash != stored->hash ||
                       info->st.st_ino != stored->inode ||
                       info->st.st_nlink != stored->link_count ||
                       info->st.st_blocks != stored->block_count)) {

                FileEntryData entry = create_entry_data (filepath, info);
                data.mv_size = sizeof(FileEntryData);
                data.mv_data = &entry;

                rc = mdb_put (txn, db_data->dbi, &key, &data, 0);
                if (rc != 0) {
                    g_log (NULL, G_LOG_LEVEL_ERROR, "mdb_put failed: %s\n", mdb_strerror (rc));
                    mdb_txn_abort (txn);
                    g_free (entry.filepath);
                    return FALSE;
                }
                g_free (entry.filepath);
                summary_data->total_files_processed++;
            }
        }
    }

    if (op == MODE_CHECK)
        mdb_txn_abort (txn);
    else
        mdb_txn_commit (txn);

    return TRUE;
}


void
handle_missing_files_from_fs (DatabaseData *db_data,
                              SummaryData  *summary_data,
                              gboolean      delete_file_from_db)
{
    MDB_txn *txn;
    MDB_cursor *cursor;
    MDB_val key, data;

    int txn_flags = delete_file_from_db ? 0 : MDB_RDONLY;
    int rc = mdb_txn_begin (db_data->env, NULL, txn_flags, &txn);
    if (rc != 0) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "mdb_txn_begin failed: %s\n", mdb_strerror (rc));
        return;
    }

    rc = mdb_cursor_open (txn, db_data->dbi, &cursor);
    if (rc == 0) {
        while (mdb_cursor_get (cursor, &key, &data, MDB_NEXT) == 0) {
            gchar *db_filepath = g_strndup (key.mv_data, key.mv_size);
            if (!g_file_test (db_filepath, G_FILE_TEST_EXISTS)) {
                if (delete_file_from_db == FALSE) {
                    record_change (summary_data, db_filepath, CHANGE_MISSING_IN_FS);
                } else {
                    rc = mdb_del (txn, db_data->dbi, &key, NULL);
                    if (rc != 0) {
                        g_log (NULL, G_LOG_LEVEL_ERROR, "mdb_del failed: %s\n", mdb_strerror (rc));
                    }
                }
            }
            g_free (db_filepath);
        }
        mdb_cursor_close (cursor);
    }

    if (delete_file_from_db) {
        mdb_txn_commit (txn);
    } else {
        mdb_txn_abort (txn);
    }
}


void
process_file (const gchar  *file_path,
              ConsumerData *consumer_data)
{
    if (!validate_filepath (file_path)) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Invalid file path: %s\n", file_path);
        return;
    }

    FileInfo info;
    if (get_file_info (file_path, consumer_data->config_data->max_ram_per_thread, &info)) {
        handle_db_operation (file_path, &info, consumer_data->db_data, consumer_data->summary_data, consumer_data->config_data->mode);
    }
}