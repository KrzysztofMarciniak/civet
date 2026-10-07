#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>

#include "lib.h"
#include "vfs.h"

#define CACHE_OK          0
#define CACHE_NOT_FOUND   1
#define CACHE_ERROR       (-1)

/*
 * A cached file is identified by its normalized virtual URL path.
 *
 * The file contents are owned by the cache entry and remain in memory
 * until the entry is destroyed.
 */
struct cache_entry {
    char *url_path;
    char *content;

    size_t size;

    struct cache_entry *next;
};

/*
 * In-memory cache state.
 *
 * total_bytes counts only cached file contents, not the small amount
 * of metadata allocated for each cache entry.
 */
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
 * Examples:
 *
 *     "index.html,other_page.html"
 *     "/index.html,/css/style.css"
 *
 * Paths are normalized through the VFS before lookup. The VFS remains
 * authoritative for the mapping from virtual paths to disk paths.
 *
 * Files are loaded completely into memory during this call.
 */
s4 cache_load(struct cache *cache,
              const struct vfs *vfs,
              const char *paths);

/*
 * Look up a cached file by normalized virtual path.
 *
 * Returns NULL when the path is not cached.
 *
 * The returned entry is owned by the cache and must not be freed by
 * the caller.
 */
const struct cache_entry *
cache_lookup(const struct cache *cache,
             const char *url_path);

/*
 * Destroy all cached entries and their contents.
 */
void cache_destroy(struct cache *cache);

#endif
