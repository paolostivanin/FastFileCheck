#include <glib.h>
#include <glib/gstdio.h>
#include "config.h"


static GMutex log_mutex;
static FILE *cached_log_file = NULL;


static void
ensure_log_file_open (const char *log_path)
{
    if (cached_log_file == NULL) {
        cached_log_file = g_fopen (log_path, "a");
        if (cached_log_file) {
            setvbuf (cached_log_file, NULL, _IOLBF, 0);
        }
    }
}


void
cleanup_logger (void)
{
    if (cached_log_file) {
        fclose (cached_log_file);
        cached_log_file = NULL;
    }
}


void
log_handler (const gchar    *log_domain __attribute__((unused)),
             GLogLevelFlags  log_level,
             const gchar    *message,
             gpointer        user_data)
{
    // Always print ERROR and WARNING messages to stderr
    if (log_level & (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_WARNING)) {
        g_printerr ("[%s] %s\n", log_level & G_LOG_LEVEL_ERROR ? "ERROR" : "WARNING", message);
    }

    // In verbose mode, also echo INFO/DEBUG/MESSAGE to stdout for live feedback
    if (user_data) {
        ConfigData *cfg = (ConfigData *)user_data;
        if (cfg->verbose) {
            GLogLevelFlags level = (log_level & G_LOG_LEVEL_MASK);
            if (level == G_LOG_LEVEL_INFO || level == G_LOG_LEVEL_DEBUG || level == G_LOG_LEVEL_MESSAGE) {
                g_print ("%s\n", message);
            }
        }
    }

    if (!user_data) return;

    ConfigData *config = (ConfigData *)user_data;
    if (!config->logging_enabled || !config->log_path) return;

    g_mutex_lock (&log_mutex);

    ensure_log_file_open (config->log_path);
    if (!cached_log_file) {
        g_mutex_unlock (&log_mutex);
        return;
    }

    GDateTime *now = g_date_time_new_now_local ();
    gchar *timestamp = g_date_time_format (now, "%Y-%m-%d %H:%M:%S");

    const gchar *level_str;
    switch (log_level & G_LOG_LEVEL_MASK) {
        case G_LOG_LEVEL_ERROR:    level_str = "ERROR"; break;
        case G_LOG_LEVEL_CRITICAL: level_str = "CRITICAL"; break;
        case G_LOG_LEVEL_WARNING:  level_str = "WARNING"; break;
        case G_LOG_LEVEL_MESSAGE:  level_str = "MESSAGE"; break;
        case G_LOG_LEVEL_INFO:     level_str = "INFO"; break;
        case G_LOG_LEVEL_DEBUG:    level_str = "DEBUG"; break;
        default:                   level_str = "UNKNOWN";
    }

    fprintf (cached_log_file, "[%s] %s: %s\n", timestamp, level_str, message);
    fflush (cached_log_file);

    g_date_time_unref (now);
    g_free (timestamp);

    g_mutex_unlock (&log_mutex);
}