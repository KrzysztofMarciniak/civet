/* cache.c - in-memory cache for static files. */

#include "cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <unistd.h>

#define CACHE_MAX_BYTES (256UL * 1024UL * 1024UL)

/* ------------------------------------------------------------------ */

static void cache_entry_free(struct cache_entry *entry)
{
    if (entry == NULL)
        return;

    free(entry->url_path);
    free(entry->content);
    free(entry);
}

/* ------------------------------------------------------------------ */

static s4 normalize_cache_path(const char *input,
                               char *output,
                               size_t output_size)
{
    char rooted[PATH_MAX];
    size_t len;

    if (input == NULL ||
        output == NULL ||
        output_size == 0)
        return CACHE_ERROR;

    if (*input == '\0')
        return CACHE_ERROR;

    if (input[0] == '/') {
        return vfs_normalize_path(input,
                                  output,
                                  output_size) == VFS_OK
            ? CACHE_OK
            : CACHE_ERROR;
    }

    len = strlen(input);

    if (len + 2 > sizeof(rooted))
        return CACHE_ERROR;

    rooted[0] = '/';

    memcpy(rooted + 1,
           input,
           len + 1);

    return vfs_normalize_path(rooted,
                              output,
                              output_size) == VFS_OK
        ? CACHE_OK
        : CACHE_ERROR;
}

/* ------------------------------------------------------------------ */

void cache_init(struct cache *cache)
{
    if (cache == NULL)
        return;

    cache->entries = NULL;
    cache->entry_count = 0;
    cache->total_bytes = 0;
}

/* ------------------------------------------------------------------ */

void cache_destroy(struct cache *cache)
{
    struct cache_entry *entry;
    struct cache_entry *next;

    if (cache == NULL)
        return;

    entry = cache->entries;

    while (entry != NULL) {
        next = entry->next;
        cache_entry_free(entry);
        entry = next;
    }

    cache->entries = NULL;
    cache->entry_count = 0;
    cache->total_bytes = 0;
}

/* ------------------------------------------------------------------ */

static s4 read_file(const struct vfs_server *vfs_server,
                    const struct vfs_entry *entry,
                    char **content,
                    size_t *size)
{
    int fd;
    struct stat st;
    char *buf;
    size_t wanted;
    size_t used;
    ssize_t n;

    if (vfs_server == NULL ||
        entry == NULL ||
        content == NULL ||
        size == NULL)
        return CACHE_ERROR;

    *content = NULL;
    *size = 0;

    fd = vfs_server_open(vfs_server, entry);

    if (fd < 0) {
        fprintf(stderr,
                "civet: cannot open cache file '%s': %s\n",
                entry->url_path,
                strerror(errno));
        return CACHE_ERROR;
    }

    if (fstat(fd, &st) != 0) {
        fprintf(stderr,
                "civet: cannot stat cache file '%s': %s\n",
                entry->url_path,
                strerror(errno));
        close(fd);
        return CACHE_ERROR;
    }

    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr,
                "civet: cache path is not a regular file: '%s'\n",
                entry->url_path);
        close(fd);
        return CACHE_ERROR;
    }

    if (st.st_size < 0) {
        fprintf(stderr,
                "civet: invalid cache file size: '%s'\n",
                entry->url_path);
        close(fd);
        return CACHE_ERROR;
    }

    if ((unsigned long)st.st_size > CACHE_MAX_BYTES) {
        fprintf(stderr,
                "civet: cache file is too large: '%s' "
                "(maximum %lu bytes)\n",
                entry->url_path,
                CACHE_MAX_BYTES);
        close(fd);
        return CACHE_ERROR;
    }

    wanted = (size_t)st.st_size;

    if (wanted == 0)
        buf = (char *)malloc(1);
    else
        buf = (char *)malloc(wanted);

    if (buf == NULL) {
        fprintf(stderr,
                "civet: cannot cache '%s': out of memory\n",
                entry->url_path);
        close(fd);
        return CACHE_ERROR;
    }

    used = 0;

    while (used < wanted) {
        n = read(fd,
                 buf + used,
                 wanted - used);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            fprintf(stderr,
                    "civet: cannot read cache file '%s': %s\n",
                    entry->url_path,
                    strerror(errno));

            free(buf);
            close(fd);
            return CACHE_ERROR;
        }

        if (n == 0) {
            fprintf(stderr,
                    "civet: unexpected EOF while caching '%s'\n",
                    entry->url_path);

            free(buf);
            close(fd);
            return CACHE_ERROR;
        }

        used += (size_t)n;
    }

    if (close(fd) != 0) {
        fprintf(stderr,
                "civet: cannot close cache file '%s': %s\n",
                entry->url_path,
                strerror(errno));

        free(buf);
        return CACHE_ERROR;
    }

    *content = buf;
    *size = used;

    return CACHE_OK;
}

/* ------------------------------------------------------------------ */

static s4 cache_add(struct cache *cache,
                    const struct vfs_server *vfs_server,
                    const struct vfs_entry *entry,
                    const char *url_path)
{
    struct cache_entry *cached;
    char *content;
    size_t size;

    if (cache == NULL ||
        vfs_server == NULL ||
        entry == NULL ||
        url_path == NULL)
        return CACHE_ERROR;

    if (entry->type != VFS_FILE)
        return CACHE_ERROR;

    if (cache_lookup(cache, url_path) != NULL)
        return CACHE_OK;

    if (entry->size < 0 ||
        (unsigned long)entry->size >
        CACHE_MAX_BYTES - cache->total_bytes) {
        fprintf(stderr,
                "civet: cache limit exceeded by '%s'\n",
                url_path);
        return CACHE_ERROR;
    }

    if (read_file(vfs_server,
                  entry,
                  &content,
                  &size) != CACHE_OK)
        return CACHE_ERROR;

    if (size > CACHE_MAX_BYTES - cache->total_bytes) {
        fprintf(stderr,
                "civet: cache limit exceeded by '%s'\n",
                url_path);
        free(content);
        return CACHE_ERROR;
    }

    cached = (struct cache_entry *)malloc(sizeof(*cached));

    if (cached == NULL) {
        free(content);

        fprintf(stderr,
                "civet: cannot create cache entry: out of memory\n");

        return CACHE_ERROR;
    }

    memset(cached, 0, sizeof(*cached));

    cached->url_path = strdup(url_path);

    if (cached->url_path == NULL) {
        free(content);
        free(cached);

        fprintf(stderr,
                "civet: cannot cache '%s': out of memory\n",
                url_path);

        return CACHE_ERROR;
    }

    cached->content = content;
    cached->size = size;

    cached->next = cache->entries;
    cache->entries = cached;

    cache->entry_count++;
    cache->total_bytes += size;

    return CACHE_OK;
}

/* ------------------------------------------------------------------ */

static s4 cache_load_one(struct cache *cache,
                         const struct vfs *vfs,
                         const struct vfs_server *vfs_server,
                         const char *input)
{
    char normalized[PATH_MAX];
    const struct vfs_entry *entry;
    const struct cache_entry *cached;

    if (normalize_cache_path(input,
                             normalized,
                             sizeof(normalized)) != CACHE_OK) {
        fprintf(stderr,
                "civet: invalid cache path: '%s'\n",
                input);
        return CACHE_ERROR;
    }

    entry = vfs_lookup(vfs, normalized);

    if (entry == NULL) {
        fprintf(stderr,
                "civet: cache file not found: '%s'\n",
                normalized);
        return CACHE_ERROR;
    }

    if (entry->type != VFS_FILE) {
        fprintf(stderr,
                "civet: cache path is not a file: '%s'\n",
                normalized);
        return CACHE_ERROR;
    }

    if (cache_lookup(cache, normalized) != NULL)
        return CACHE_OK;

    if (cache_add(cache,
                  vfs_server,
                  entry,
                  normalized) != CACHE_OK)
        return CACHE_ERROR;

    cached = cache_lookup(cache, normalized);

    if (cached == NULL)
        return CACHE_ERROR;

    fprintf(stderr,
            "civet: cached %s (%lu bytes)\n",
            normalized,
            (unsigned long)cached->size);

    return CACHE_OK;
}

/* ------------------------------------------------------------------ */

s4 cache_load(struct cache *cache,
              const struct vfs *vfs,
              const struct vfs_server *vfs_server,
              const char *paths)
{
    char *list;
    char *token;

    if (cache == NULL ||
        vfs == NULL ||
        vfs_server == NULL)
        return CACHE_ERROR;

    if (paths == NULL || *paths == '\0')
        return CACHE_OK;

    list = strdup(paths);

    if (list == NULL) {
        fprintf(stderr,
                "civet: cannot parse cache list: out of memory\n");
        return CACHE_ERROR;
    }

    token = strtok(list, ",");

    while (token != NULL) {
        char *end;

        while (*token == ' ' ||
               *token == '\t')
            token++;

        end = token + strlen(token);

        while (end > token &&
               (end[-1] == ' ' ||
                end[-1] == '\t'))
            end--;

        *end = '\0';

        if (*token == '\0') {
            fprintf(stderr,
                    "civet: empty cache path\n");
            free(list);
            return CACHE_ERROR;
        }

        if (cache_load_one(cache,
                           vfs,
                           vfs_server,
                           token) != CACHE_OK) {
            free(list);
            return CACHE_ERROR;
        }

        token = strtok(NULL, ",");
    }

    free(list);

    fprintf(stderr,
            "civet: cache contains %lu files (%lu bytes, %lu MiB max)\n",
            (unsigned long)cache->entry_count,
            (unsigned long)cache->total_bytes,
            (unsigned long)(CACHE_MAX_BYTES /
                            (1024UL * 1024UL)));

    return CACHE_OK;
}

/* ------------------------------------------------------------------ */

const struct cache_entry *
cache_lookup(const struct cache *cache,
             const char *url_path)
{
    const struct cache_entry *entry;

    if (cache == NULL ||
        url_path == NULL)
        return NULL;

    for (entry = cache->entries;
         entry != NULL;
         entry = entry->next) {

        if (strcmp(entry->url_path,
                   url_path) == 0)
            return entry;
    }

    return NULL;
}
