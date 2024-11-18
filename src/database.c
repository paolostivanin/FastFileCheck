#include <lmdb.h>
#include <stdlib.h>
#include <stdio.h>
#include "config.h"
#include "database.h"

DatabaseData *
init_db (ConfigData *config_data)
{
    DatabaseData *db_data = (DatabaseData *)calloc (1, sizeof(DatabaseData));

    int rc = mdb_env_create (&db_data->env);
    if (rc != 0) {
        perror ("Error in mdb_env_create");
        free (db_data);
        return NULL;
    }

    rc = mdb_env_set_mapsize (db_data->env, config_data->db_size_bytes);
    if (rc != 0) {
        perror ("Error in mdb_env_set_mapsize");
        free (db_data);
        return NULL;
    }

    rc = mdb_env_open (db_data->env, config_data->db_path, 0, 0664);
    if (rc != 0) {
        perror ("Error in mdb_env_open");
        free (db_data);
        return NULL;
    }

    MDB_txn *txn;
    rc = mdb_txn_begin (db_data->env, NULL, 0, &txn);
    if (rc != 0) {
        perror ("Error in mdb_txn_begin");
        free (db_data);
        return NULL;
    }

    rc = mdb_dbi_open (txn, NULL, 0, &db_data->dbi);
    if (rc != 0) {
        perror ("Error in mdb_dbi_open");
        free (db_data);
        return NULL;
    }
    mdb_txn_commit (txn);

    return db_data;
}