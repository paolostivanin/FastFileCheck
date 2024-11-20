#pragma once

#define MAX_FILENAME_LEN 256

typedef struct database_t {
    MDB_env *env;
    MDB_dbi dbi;
} DatabaseData;

DatabaseData *init_db (ConfigData *config_data);