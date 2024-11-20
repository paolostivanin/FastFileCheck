#pragma once

typedef struct database_t {
    MDB_env *env;
    MDB_dbi dbi;
} DatabaseData;

DatabaseData *init_db (ConfigData *config_data);

void free_db (DatabaseData *db_data);