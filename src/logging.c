#include <glib.h>
#include <glib/gstdio.h>
#include "config.h"

void
log_handler (const gchar    *log_domain __attribute__((unused)),
             GLogLevelFlags  log_level,
             const gchar    *message,
             gpointer        user_data)
{
    ConfigData *config = (ConfigData *)user_data;
    if (!config->logging_enabled) return;

    GDateTime *now = g_date_time_new_now_local ();
    gchar *timestamp = g_date_time_format (now, "%Y-%m-%d %H:%M:%S");

    const gchar *level_str;
    switch (log_level) {
        case G_LOG_LEVEL_ERROR:    level_str = "ERROR"; break;
        case G_LOG_LEVEL_WARNING:  level_str = "WARNING"; break;
        case G_LOG_LEVEL_INFO:     level_str = "INFO"; break;
        case G_LOG_LEVEL_DEBUG:    level_str = "DEBUG"; break;
        default:                   level_str = "UNKNOWN";
    }

    FILE *log_file = g_fopen (config->log_path, "a");
    if (log_file) {
        fprintf (log_file, "[%s] %s: %s\n", timestamp, level_str, message);
        fclose (log_file);
    }

    g_date_time_unref (now);
    g_free (timestamp);
}