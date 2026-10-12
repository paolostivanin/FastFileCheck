#include <glib.h>
#include <string.h>
#include <xxhash.h>
#include "seen_set.h"

#define SEEN_SHARDS 64  // power of two; shard picked from the top hash bits

// The 64-bit hash is stored directly in the pointer-sized key
G_STATIC_ASSERT (sizeof (gpointer) == sizeof (guint64));

typedef struct seen_shard_t {
    GMutex      mutex;
    GHashTable *table;
} SeenShard;

struct seen_set_t {
    SeenShard shards[SEEN_SHARDS];
};


static inline guint64
hash_path (const gchar *path,
           gsize        len)
{
    return (guint64)XXH3_64bits (path, len);
}


static inline SeenShard *
pick_shard (SeenSet *set,
            guint64  hash)
{
    return &set->shards[hash >> 58];
}


SeenSet *
seen_set_new (void)
{
    SeenSet *set = g_try_new0 (SeenSet, 1);
    if (!set) return NULL;

    for (guint i = 0; i < SEEN_SHARDS; i++) {
        g_mutex_init (&set->shards[i].mutex);
        set->shards[i].table = g_hash_table_new (g_direct_hash, g_direct_equal);
    }
    return set;
}


void
seen_set_free (SeenSet *set)
{
    if (!set) return;

    for (guint i = 0; i < SEEN_SHARDS; i++) {
        g_hash_table_destroy (set->shards[i].table);
        g_mutex_clear (&set->shards[i].mutex);
    }
    g_free (set);
}


void
seen_set_add (SeenSet     *set,
              const gchar *path)
{
    guint64 hash = hash_path (path, strlen (path));
    SeenShard *shard = pick_shard (set, hash);

    g_mutex_lock (&shard->mutex);
    g_hash_table_add (shard->table, GSIZE_TO_POINTER ((gsize)hash));
    g_mutex_unlock (&shard->mutex);
}


gboolean
seen_set_contains (SeenSet     *set,
                   const gchar *path,
                   gsize        path_len)
{
    guint64 hash = hash_path (path, path_len);
    SeenShard *shard = pick_shard (set, hash);

    g_mutex_lock (&shard->mutex);
    gboolean found = g_hash_table_contains (shard->table, GSIZE_TO_POINTER ((gsize)hash));
    g_mutex_unlock (&shard->mutex);
    return found;
}
