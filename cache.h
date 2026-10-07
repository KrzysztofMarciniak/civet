#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>
#include <sys/types.h>

#include "lib.h"
#include "vfs.h"

#define CACHE_OK          0
#define CACHE_NOT_FOUND   1
#define CACHE_ERROR       (-1)

/*
 * A cached file is identified by its virtual URL path.
 *
 * The file contents are kept entirely in memory.
 */
struct cache_entry {
    char *url_path;
    char *content;

    size_t size;

    struct cache_entry *next;
};

struct cache {
    struct cache_entry *entries;
    size_t entry_count;
    size_t total_bytes;
};

/*
 * Initialize an empty cache.
 */
void cache_init(struct cache *cache);

/*
 * Load comma-separated virtual paths into the cache.
 *
 * Example:
 *
 *     cache_load(&cache, vfs, "index.html,other_page.html");
 *
 * Paths are normalized before lookup.
 */
s4 cache_load(struct cache *cache,
              const struct vfs *vfs,
              const char *paths);

/*
 * Look up a cached file by virtual path.
 */
const struct cache_entry *
cache_lookup(const struct cache *cache,
             const char *url_path);

/*
 * Destroy all cached file contents.
 */
void cache_destroy(struct cache *cache);

#endif
