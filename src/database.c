#include <lmdb.h>
#include <stdlib.h>
#include <stdio.h>
#include "config.h"
#include "database.h"


void
free_db (DatabaseData *db_data)
{
    if (db_data) {
        if (db_data->env && db_data->dbi) mdb_dbi_close (db_data->env, db_data->dbi);
        if (db_data->env) mdb_env_close (db_data->env);
        g_free (db_data);
    }
}


DatabaseData *
init_db (ConfigData *config_data)
{
    if (!config_data || !config_data->db_path) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Invalid configuration data");
        return NULL;
    }

    DatabaseData *db_data = g_try_new0 (DatabaseData, 1);
    if (!db_data) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Failed to allocate memory for DatabaseData");
        return NULL;
    }

    int rc = mdb_env_create (&db_data->env);
    if (rc != 0) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Error in mdb_env_create: %s", mdb_strerror (rc));
        g_free (db_data);
        return NULL;
    }

    rc = mdb_env_set_mapsize (db_data->env, config_data->db_size_bytes);
    if (rc != 0) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Error in mdb_env_set_mapsize: %s", mdb_strerror (rc));
        g_free (db_data);
        return NULL;
    }

    rc = mdb_env_open (db_data->env, config_data->db_path, 0, 0644);
    if (rc != 0) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Error in mdb_env_open: %s", mdb_strerror (rc));
        mdb_env_close (db_data->env);
        g_free (db_data);
        return NULL;
    }

    MDB_txn *txn;
    rc = mdb_txn_begin (db_data->env, NULL, 0, &txn);
    if (rc != 0) {
        g_log (NULL, G_LOG_LEVEL_ERROR, "Error in mdb_txn_begin: %s", mdb_strerror (rc));
        g_free (db_data);
        return NULL;
    }

    rc = mdb_dbi_open (txn, NULL, 0, &db_data->dbi);
    if (rc != 0) {
        mdb_txn_abort (txn);
        g_log (NULL, G_LOG_LEVEL_ERROR, "Error in mdb_dbi_open: %s", mdb_strerror (rc));
        g_free (db_data);
        return NULL;
    }
    mdb_txn_commit (txn);

    return db_data;
}