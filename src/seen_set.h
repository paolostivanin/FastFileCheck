#pragma once

#include <glib.h>

// Thread-safe set of the paths that the workers have looked at during this run.
// It stores a 64-bit hash of each path (no copy of the string), so ~16-24 bytes per file.
//
// The missing-file pass uses it to avoid one stat() per database entry: only entries that
// are NOT in the set (deleted files, excluded directories, hash-collision victims) need a
// real filesystem check. A 64-bit collision can only make a file look "seen", i.e. a
// deleted file might be reported one run later; it can never produce a false report.
typedef struct seen_set_t SeenSet;

SeenSet  *seen_set_new      (void);

void      seen_set_free     (SeenSet     *set);

void      seen_set_add      (SeenSet     *set,
                             const gchar *path);

gboolean  seen_set_contains (SeenSet     *set,
                             const gchar *path,
                             gsize        path_len);
