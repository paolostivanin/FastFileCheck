#include <stdio.h>
#include <string.h>
#include "config.h"
#include "database.h"

static void
usage (const char *prog_name)
{
    fprintf(stderr, "Usage: %s [--config /path/to/cfg] <add|check> <directory_path>\n", prog_name);
}


int main(int argc, char *argv[]) {
    const char *config_path = NULL;
    if (argc == 4 && strcmp(argv[1], "--config") == 0) {
        config_path = argv[2];
    }

    if (argc != 3 && argc != 5) {
        usage(argv[0]);
        return -1;
    }

    ConfigData *config_data = load_config (config_path);
    config_data->mode = (strcmp (argv[1], "add") == 0 || strcmp (argv[2], "add") == 0) ? MODE_ADD : MODE_CHECK;

    DatabaseData *db_data = init_db (config_data);
    if (db_data == NULL) return -1;

    free_config (config_data);

    return 0;
}