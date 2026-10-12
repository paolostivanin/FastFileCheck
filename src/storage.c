#include <glib.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "storage.h"

#define MOUNTINFO_PATH "/proc/self/mountinfo"


// mountinfo escapes space, tab, newline and backslash as \040 \011 \012 \134
static gchar *
unescape_mountinfo (const gchar *field)
{
    GString *out = g_string_new (NULL);
    for (const gchar *p = field; *p; p++) {
        if (p[0] == '\\' && g_ascii_isdigit (p[1]) && g_ascii_isdigit (p[2]) && g_ascii_isdigit (p[3])) {
            g_string_append_c (out, (gchar)(((p[1] - '0') << 6) | ((p[2] - '0') << 3) | (p[3] - '0')));
            p += 3;
        } else {
            g_string_append_c (out, *p);
        }
    }
    return g_string_free (out, FALSE);
}


static gboolean
is_path_prefix (const gchar *mount_point,
                const gchar *path)
{
    gsize len = strlen (mount_point);
    if (strcmp (mount_point, "/") == 0) return TRUE;
    return strncmp (path, mount_point, len) == 0 && (path[len] == '\0' || path[len] == '/');
}


// Source device (e.g. /dev/sdb3) of the mount that contains `real_path`, or NULL
static gchar *
find_mount_source (const gchar *real_path)
{
    FILE *f = fopen (MOUNTINFO_PATH, "re");
    if (!f) return NULL;

    gchar *best_source = NULL;
    gsize best_len = 0;
    gboolean found = FALSE;
    gchar *line = NULL;
    size_t cap = 0;

    while (getline (&line, &cap, f) > 0) {
        // 36 35 98:0 /root /mnt/point opts [optional fields...] - fstype source super_opts
        gchar **fields = g_strsplit (g_strchomp (line), " ", -1);
        guint n = g_strv_length (fields);

        guint sep = 0;
        for (guint i = 6; i < n; i++) {
            if (strcmp (fields[i], "-") == 0) { sep = i; break; }
        }

        if (sep > 0 && sep + 2 < n) {
            gchar *mount_point = unescape_mountinfo (fields[4]);
            gsize len = strlen (mount_point);
            // later lines win on equal length (a newer mount hides an older one)
            if (is_path_prefix (mount_point, real_path) && (!found || len >= best_len)) {
                g_free (best_source);
                best_source = unescape_mountinfo (fields[sep + 2]);
                best_len = len;
                found = TRUE;
            }
            g_free (mount_point);
        }
        g_strfreev (fields);
    }

    free (line);
    fclose (f);
    return best_source;
}


static gboolean
read_rotational_flag (const gchar *queue_dir,
                      gboolean    *rotational)
{
    gchar *file = g_build_filename (queue_dir, "rotational", NULL);
    gchar *contents = NULL;
    gboolean ok = g_file_get_contents (file, &contents, NULL, NULL);
    if (ok) *rotational = (contents[0] == '1');
    g_free (contents);
    g_free (file);
    return ok;
}


gboolean
storage_path_is_rotational (const gchar *path)
{
    gboolean rotational = FALSE;
    char resolved[PATH_MAX];
    if (realpath (path, resolved) == NULL) {
        return FALSE;
    }

    gchar *source = find_mount_source (resolved);
    if (!source || !g_str_has_prefix (source, "/dev/")) {
        g_free (source);
        return FALSE;  // tmpfs, NFS, overlay, FUSE, ...
    }

    // /dev/mapper/foo and /dev/disk/by-uuid/... are symlinks to the real node
    char dev_real[PATH_MAX];
    const gchar *dev_path = realpath (source, dev_real) ? dev_real : source;
    gchar *name = g_path_get_basename (dev_path);
    g_free (source);

    // /sys/class/block/<name> is a symlink into /sys/devices/...; a partition has no queue/
    // of its own, its parent disk (one directory up) does.
    gchar *class_path = g_build_filename ("/sys/class/block", name, NULL);
    char sys_real[PATH_MAX];
    if (realpath (class_path, sys_real) != NULL) {
        gchar *own_queue = g_build_filename (sys_real, "queue", NULL);
        if (!read_rotational_flag (own_queue, &rotational)) {
            gchar *parent_queue = g_build_filename (sys_real, "..", "queue", NULL);
            read_rotational_flag (parent_queue, &rotational);
            g_free (parent_queue);
        }
        g_free (own_queue);
    }

    g_free (class_path);
    g_free (name);
    return rotational;
}
