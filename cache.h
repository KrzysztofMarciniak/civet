#ifndef CACHE_H
#define CACHE_H

#include <stddef.h>

#include "lib.h"
#include "vfs.h"
#include "vfs_server.h"

#define CACHE_OK 0
#define CACHE_NOT_FOUND 1
#define CACHE_ERROR (-1)

struct cache_entry {
        char* url_path;
        char* content;
        size_t size;
        struct cache_entry* next;
};

struct cache {
        struct cache_entry* entries;
        size_t entry_count;
        size_t total_bytes;
};

void cache_init(struct cache* cache);

s4 cache_load(struct cache* cache, const struct vfs* vfs,
              const struct vfs_server* vfs_server, const char* paths);

const struct cache_entry* cache_lookup(const struct cache* cache,
                                       const char* url_path);

void cache_destroy(struct cache* cache);

#endif
