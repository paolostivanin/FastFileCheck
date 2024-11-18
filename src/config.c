#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pwd.h>
#include <unistd.h>
#include <ctype.h>
#include "config.h"


#define MAX_LINE_LENGTH     256
#define MAX_KEY_LENGTH       48
#define MAX_VALUE_LENGTH    208

static void
trim (char *str)
{
    char *end = str + strlen (str) - 1;
    while (isspace ((unsigned char)*str)) str++;
    while (end > str && isspace ((unsigned char)*end)) end--;
    *(end + 1) = '\0';
}


static void
collapse_spaces (char *str)
{
    char *src = str, *dst = str;
    int in_space = 0;

    while (*src) {
        if (isspace ((unsigned char)*src)) {
            if (!in_space) {
                *dst++ = ' ';
                in_space = 1;
            }
        } else {
            *dst++ = *src;
            in_space = 0;
        }
        src++;
    }
    *dst = '\0';
}


static int
read_config (const char *file_path, Config *config)
{
    FILE *file = fopen (file_path, "r");
    if (file == NULL) {
        perror("Failed to open config file, using defaults");
        return -1;
    }

    char line[MAX_LINE_LENGTH];
    char key[MAX_KEY_LENGTH], value[MAX_VALUE_LENGTH];
    while (fgets(line, sizeof(line), file)) {
        trim (line);  // remove leading and trailing whitespace
        if (line[0] == '\0' || line[0] == ';' || line[0] == '#') {
            // skip empty lines and comments
            continue;
        }
        collapse_spaces (line);

        // parse the key-value pair, allowing zero or one spaces around the equal sign
        if (sscanf(line, "%47[^=] = %207[^\n]", key, value) == 2 ||
            sscanf(line, "%47[^=]=%207[^\n]", key, value) == 2) {
            trim(key);
            trim(value);

            // Assign values to the Config struct based on the key
            char *endptr;
            unsigned long t_val;
            if (strcmp (key, "threads") == 0) {
                 t_val = strtoul (value, &endptr, 10);
                if (*endptr != '\0') {
                    printf ("Invalid number for threads: %s. Using the default value instead.\n", value);
                } else {
                    // if t_val is greater than 'all threads', then we keep the default value
                    if (t_val > (config->threads + 1)) {
                        fprintf(stderr, "Invalid threads number: %lu. Using the default value instead.\n", t_val);
                    }
                    config->threads = (t_val > (config->threads + 1)) ? config->threads : t_val;
                }
            } else if (strcmp (key, "ram_usage_percent") == 0) {
                t_val = strtoul (value, &endptr, 10);
                if (*endptr != '\0') {
                    printf ("Invalid number for ram_usage_percent: %s. Using the default value instead.\n", value);
                } else {
                    if (t_val < 10 || t_val > 90) {
                        printf ("Invalid ram_usage_percent value: %lu. Using the default value instead.\n", t_val);
                        t_val = DEFAULT_RAM_USAGE_PERCENT;
                    }
                    config->usable_ram = (sysconf(_SC_AVPHYS_PAGES) * sysconf(_SC_PAGE_SIZE) * t_val) / 100;
                }
            } else if (strcmp (key, "db_size_mb") == 0) {
                t_val = strtoul (value, &endptr, 10);
                if (*endptr != '\0') {
                    printf ("Invalid number for db_size_mb: %s. Using the default value instead.\n", value);
                } else {
                    config->db_size_mb = t_val;
                }
            } else if (strcmp (key, "db_path") == 0 || strcmp (key, "log_path") == 0) {
                if (strlen (value) > MAX_VALUE_LENGTH) {
                    printf ("Invalid path for %s: %s. Using the default value instead.\n", key, value);
                } else {
                    if (strcmp (key, "db_path") == 0) {
                        config->db_path = strdup (value);
                    } else {
                        config->log_path = strdup (value);
                    }
                }
            }
        }
    }
    fclose(file);
    return 0;
}


Config *
load_config (const char *config_path)
{
    Config *config_data = (Config *)calloc (1, sizeof(Config));

    // If no config was specified, we use the default one
    config_path = (config_path == NULL) ? DEFAULT_CONFIG_PATH : config_path;

    // Set default configuration values
    config_data->threads = sysconf(_SC_NPROCESSORS_ONLN) - 1;
    config_data->usable_ram = (sysconf(_SC_AVPHYS_PAGES) * sysconf(_SC_PAGE_SIZE) * DEFAULT_RAM_USAGE_PERCENT) / 100;
    config_data->db_size_mb = DEFAULT_DB_SIZE_IN_BYTES;
    config_data->db_path = strdup (DEFAULT_DB_PATH);
    config_data->log_path = strdup (DEFAULT_LOG_PATH);

    FILE *file = fopen (config_path, "r");
    if (!file) {
        printf ("Config file not found at %s. Using the default settings.\n", config_path);
    } else {
        read_config (config_path, config_data);
        fclose (file);
    }

    return config_data;
}


void
free_config (Config *config)
{
    if (config) {
        free (config->db_path);
        free (config->log_path);
        free (config);
    }
}