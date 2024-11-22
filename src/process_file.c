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


static guint64
compute_hash (const char *filepath,
              const guint64 per_thread_ram)
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


static void
add_to_db (const char *filepath,
           const guint64 per_thread_ram,
           DatabaseData *db_data)
{
    struct stat st;
    if (stat (filepath, &st) != 0) {
        g_print ("Could not stat file: %s\n", filepath);
        return;
    }

    const guint64 hash = compute_hash (filepath, per_thread_ram);
    if (hash == 0) {
        g_print ("Could not compute hash for file: %s\n", filepath);
        return;
    }

    FileEntryData file_entry_data = {
        .filepath = g_strdup (filepath),
        .hash = hash,
        .inode = st.st_ino,
        .link_count = st.st_nlink,
        .block_count = st.st_blocks
    };

    MDB_txn *txn;
    MDB_val key, data;
    int rc = mdb_txn_begin (db_data->env, NULL, 0, &txn);
    if (rc != 0) {
        g_print ("mdb_txn_begin failed: %s\n", mdb_strerror (rc));
        g_free (file_entry_data.filepath);
        return;
    }

    key.mv_size = g_utf8_strlen (filepath, -1) + 1;
    key.mv_data = (void*)filepath;
    data.mv_size = sizeof(FileEntryData);
    data.mv_data = &file_entry_data;

    rc = mdb_put (txn, db_data->dbi, &key, &data, 0);
    if (rc != 0) {
        g_print ("mdb_put failed: %s\n", mdb_strerror (rc));
        mdb_txn_abort (txn);
        g_free (file_entry_data.filepath);
        return;
    }

    mdb_txn_commit (txn);
    g_free (file_entry_data.filepath);
}


static void
check_db (const char *filepath,
          const guint64 per_thread_ram,
          DatabaseData *db_data)
{
    struct stat st;
    if (stat (filepath, &st) != 0) {
        g_print ("Could not stat file: %s\n", filepath);
        return;
    }

    const guint64 current_hash = compute_hash (filepath, per_thread_ram);
    if (current_hash == 0) {
        g_print ("Could not compute hash for file: %s\n", filepath);
        return;
    }

    MDB_txn *txn;
    MDB_val key, data;
    int rc = mdb_txn_begin (db_data->env, NULL, MDB_RDONLY, &txn);
    if (rc != 0) {
        g_print ("mdb_txn_begin failed: %s\n", mdb_strerror (rc));
        return;
    }

    key.mv_size = g_utf8_strlen (filepath, -1) + 1;
    key.mv_data = (void*)filepath;

    rc = mdb_get (txn, db_data->dbi, &key, &data);
    if (rc != 0) {
        g_print ("File not found in database: %s\n", filepath);
        mdb_txn_abort (txn);
        return;
    }

    FileEntryData *stored_data = (FileEntryData *)data.mv_data;

    if (current_hash != stored_data->hash) {
        g_print ("Hash mismatch for %s\n", filepath);
    }
    if (st.st_ino != stored_data->inode) {
        g_print ("Inode changed for %s\n", filepath);
    }
    if (st.st_nlink != stored_data->link_count) {
        g_print ("Link count changed for %s\n", filepath);
    }
    if (st.st_blocks != stored_data->block_count) {
        g_print ("Block count changed for %s\n", filepath);
    }

    mdb_txn_abort (txn);
}


static void
update_db (const char *filepath,
           const guint64 per_thread_ram,
           DatabaseData *db_data)
{
    struct stat st;
    if (stat (filepath, &st) != 0) {
        g_print ("Could not stat file: %s\n", filepath);
        return;
    }

    const guint64 current_hash = compute_hash (filepath, per_thread_ram);
    if (current_hash == 0) {
        g_print ("Could not compute hash for file: %s\n", filepath);
        return;
    }

    MDB_txn *txn;
    MDB_val key, data;
    int rc = mdb_txn_begin (db_data->env, NULL, 0, &txn);
    if (rc != 0) {
        g_print ("mdb_txn_begin failed: %s\n", mdb_strerror (rc));
        return;
    }

    key.mv_size = g_utf8_strlen (filepath, -1) + 1;
    key.mv_data = (void*)filepath;

    rc = mdb_get (txn, db_data->dbi, &key, &data);
    if (rc == MDB_NOTFOUND) {
        // File not in DB - add it
        FileEntryData file_entry_data = {
            .filepath = g_strdup (filepath),
            .hash = current_hash,
            .inode = st.st_ino,
            .link_count = st.st_nlink,
            .block_count = st.st_blocks
        };

        data.mv_size = sizeof(FileEntryData);
        data.mv_data = &file_entry_data;

        rc = mdb_put (txn, db_data->dbi, &key, &data, 0);
        if (rc != 0) {
            g_print ("mdb_put failed: %s\n", mdb_strerror (rc));
            mdb_txn_abort (txn);
            g_free (file_entry_data.filepath);
            return;
        }
        g_free (file_entry_data.filepath);
    } else if (rc == 0) {
        // File exists - check if update needed
        FileEntryData *stored_data = (FileEntryData *)data.mv_data;
        if (current_hash != stored_data->hash ||
            st.st_ino != stored_data->inode ||
            st.st_nlink != stored_data->link_count ||
            st.st_blocks != stored_data->block_count) {

            FileEntryData file_entry_data = {
                .filepath = g_strdup (filepath),
                .hash = current_hash,
                .inode = st.st_ino,
                .link_count = st.st_nlink,
                .block_count = st.st_blocks
            };

            data.mv_size = sizeof(FileEntryData);
            data.mv_data = &file_entry_data;

            rc = mdb_put (txn, db_data->dbi, &key, &data, 0);
            if (rc != 0) {
                g_print ("mdb_put failed: %s\n", mdb_strerror (rc));
                mdb_txn_abort (txn);
                g_free (file_entry_data.filepath);
                return;
            }
            g_free (file_entry_data.filepath);
        }
    } else {
        g_print ("mdb_get failed: %s\n", mdb_strerror (rc));
        mdb_txn_abort (txn);
        return;
    }
    mdb_txn_commit (txn);
}


void
process_file (const gchar  *file_path,
              ConsumerData *consumer_data)
{
    if (consumer_data->config_data->mode == MODE_ADD) {
        add_to_db (file_path, consumer_data->config_data->max_ram_per_thread, consumer_data->db_data);
    } else if (consumer_data->config_data->mode == MODE_CHECK) {
        check_db (file_path, consumer_data->config_data->max_ram_per_thread, consumer_data->db_data);
    } else if (consumer_data->config_data->mode == MODE_UPDATE) {
        update_db (file_path, consumer_data->config_data->max_ram_per_thread, consumer_data->db_data);
    } else {
        g_print ("Unknown mode: %d\n", consumer_data->config_data->mode);
    }
}