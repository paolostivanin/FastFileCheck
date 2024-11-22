#include <glib.h>
#include <gio/gio.h>
#include <sys/stat.h>
#include <xxhash.h>
#include "queue.h"

#define MMAP_THRESHOLD_RATIO 0.75

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


static guint64
compute_hash (const char    *filepath,
              const guint64  per_thread_ram)
{
    GError *error = NULL;
    GFile *file = g_file_new_for_path (filepath);
    GFileInfo *file_info = g_file_query_info (file, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, NULL, &error);
    if (!file_info) {
        g_print ("Failed to query file info: %s\n", error->message);
        g_clear_error (&error);
        g_object_unref (file);
        return 0;
    }

    const goffset file_size = g_file_info_get_size (file_info);
    g_object_unref (file_info);

    // Use memory mapping if file size is less than 75% of per-thread RAM
    if (file_size > 0 && (gdouble)file_size < ((gdouble)per_thread_ram * MMAP_THRESHOLD_RATIO)) {
        GMappedFile *mapped = g_mapped_file_new (filepath, FALSE, &error);
        if (mapped) {
            const gchar *contents = g_mapped_file_get_contents (mapped);
            gsize length = g_mapped_file_get_length (mapped);
            XXH64_hash_t hash = XXH3_64bits (contents, length);
            g_mapped_file_unref (mapped);
            g_object_unref (file);
            return hash;
        }
        g_print ("Failed to map file %s (%s). Falling back to chunked reading.\n", filepath, error->message);
        g_clear_error (&error);
    }

    // Fall back to chunked reading
    GFileInputStream *input_stream = g_file_read (file, NULL, &error);
    if (!input_stream) {
        g_print ("Failed to open file (%s) for reading: %s\n", filepath, error->message);
        g_clear_error (&error);
        g_object_unref (file);
        return 0;
    }

    const gsize buffer_size = CLAMP(per_thread_ram / 4,
                                     10 * 1024 * 1024,
                                    128 * 1024 * 1024);

    guchar *buffer = g_try_malloc0 (buffer_size);
    if (!buffer) {
        g_print ("Failed to allocate buffer for file %s\n", filepath);
        g_object_unref (input_stream);
        g_object_unref (file);
        return 0;
    }

    XXH3_state_t *state = XXH3_createState ();
    if (!state) {
        g_print ("Failed to create XXH3 state for file %s\n", filepath);
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
        g_print ("Could not stat file: %s\n", filepath);
        return FALSE;
    }

    info->hash = compute_hash (filepath, per_thread_ram);
    if (info->hash == 0) {
        g_print ("Could not compute hash for file: %s\n", filepath);
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
                     Mode            op)
{
    MDB_txn *txn;
    MDB_val key, data;
    int flags = (op == MODE_ADD) ? MDB_RDONLY : 0;

    int rc = mdb_txn_begin (db_data->env, NULL, flags, &txn);
    if (rc != 0) {
        g_print ("mdb_txn_begin failed: %s\n", mdb_strerror (rc));
        return FALSE;
    }

    key.mv_size = g_utf8_strlen (filepath, -1) + 1;
    key.mv_data = (void*)filepath;

    if (op == MODE_ADD) {
        FileEntryData entry = create_entry_data (filepath, info);
        data.mv_size = sizeof(FileEntryData);
        data.mv_data = &entry;

        rc = mdb_put (txn, db_data->dbi, &key, &data, 0);
        if (rc != 0) {
            g_print ("mdb_put failed: %s\n", mdb_strerror (rc));
            mdb_txn_abort (txn);
            g_free(entry.filepath);
            return FALSE;
        }
        g_free(entry.filepath);
    } else {
        rc = mdb_get (txn, db_data->dbi, &key, &data);
        if (rc != 0) {
            if (rc != MDB_NOTFOUND || op != MODE_UPDATE) {
                g_print ("File not found in database: %s\n", filepath);
                mdb_txn_abort (txn);
                return FALSE;
            }

            // For update operation, add if not found
            FileEntryData entry = create_entry_data (filepath, info);
            data.mv_size = sizeof(FileEntryData);
            data.mv_data = &entry;

            rc = mdb_put (txn, db_data->dbi, &key, &data, 0);
            if (rc != 0) {
                g_print ("mdb_put failed: %s\n", mdb_strerror (rc));
                mdb_txn_abort (txn);
                g_free(entry.filepath);
                return FALSE;
            }
            g_free(entry.filepath);
        } else {
            FileEntryData *stored = (FileEntryData *)data.mv_data;
            if (op == MODE_CHECK) {
                if (info->hash != stored->hash)
                    g_print ("Hash mismatch for %s\n", filepath);
                if (info->st.st_ino != stored->inode)
                    g_print ("Inode changed for %s\n", filepath);
                if (info->st.st_nlink != stored->link_count)
                    g_print ("Link count changed for %s\n", filepath);
                if (info->st.st_blocks != stored->block_count)
                    g_print ("Block count changed for %s\n", filepath);
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
                    g_print ("mdb_put failed: %s\n", mdb_strerror (rc));
                    mdb_txn_abort (txn);
                    g_free (entry.filepath);
                    return FALSE;
                }
                g_free (entry.filepath);
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
process_file (const gchar  *file_path,
              ConsumerData *consumer_data)
{
    FileInfo info;
    if (get_file_info (file_path, consumer_data->config_data->max_ram_per_thread, &info)) {
        handle_db_operation (file_path, &info, consumer_data->db_data, consumer_data->config_data->mode);
    }
}