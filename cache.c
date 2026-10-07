/* cache.c - in-memory cache for static files. */
#include "cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Internal helpers.                                                   */
/* ------------------------------------------------------------------ */

static char *cache_strdup(const char *s)
{
    char *p;
    size_t len;

    if (s == NULL)
        return NULL;

    len = strlen(s);

    p = (char *)malloc(len + 1);

    if (p == NULL)
        return NULL;

    memcpy(p, s, len + 1);

    return p;
}

/* ------------------------------------------------------------------ */
/* Initialize cache.                                                   */
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
/* Destroy cache.                                                      */
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

        free(entry->url_path);
        free(entry->content);
        free(entry);

        entry = next;
    }

    cache->entries = NULL;
    cache->entry_count = 0;
    cache->total_bytes = 0;
}

/* ------------------------------------------------------------------ */
/* Read an entire regular file into memory.                            */
/* ------------------------------------------------------------------ */

static s4 read_file(const char *path,
                    char **content,
                    size_t *size)
{
    int fd;
    struct stat st;
    char *buf;
    size_t used;
    ssize_t n;

    if (content == NULL || size == NULL)
        return CACHE_ERROR;

    *content = NULL;
    *size = 0;

    fd = open(path, O_RDONLY);

    if (fd < 0) {
        fprintf(stderr,
                "civet: cannot open cache file '%s': %s\n",
                path,
                strerror(errno));
        return CACHE_ERROR;
    }

    if (fstat(fd, &st) != 0) {
        fprintf(stderr,
                "civet: cannot stat cache file '%s': %s\n",
                path,
                strerror(errno));
        close(fd);
        return CACHE_ERROR;
    }

    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr,
                "civet: cache path is not a regular file: '%s'\n",
                path);
        close(fd);
        return CACHE_ERROR;
    }

    if (st.st_size < 0) {
        close(fd);
        return CACHE_ERROR;
    }

    /*
     * We allocate one extra byte so the cached contents can also be
     * treated as a C string when useful. The HTTP response still uses
     * the exact size stored in the cache entry.
     */
    if ((unsigned long)st.st_size >
        (unsigned long)((size_t)-1) - 1UL) {
        fprintf(stderr,
                "civet: cache file is too large: '%s'\n",
                path);
        close(fd);
        return CACHE_ERROR;
    }

    buf = (char *)malloc((size_t)st.st_size + 1);

    if (buf == NULL) {
        fprintf(stderr,
                "civet: cannot cache '%s': out of memory\n",
                path);
        close(fd);
        return CACHE_ERROR;
    }

    used = 0;

    while (used < (size_t)st.st_size) {
        n = read(fd,
                 buf + used,
                 (size_t)st.st_size - used);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            fprintf(stderr,
                    "civet: cannot read cache file '%s': %s\n",
                    path,
                    strerror(errno));

            free(buf);
            close(fd);
            return CACHE_ERROR;
        }

        if (n == 0) {
            fprintf(stderr,
                    "civet: unexpected EOF while caching '%s'\n",
                    path);

            free(buf);
            close(fd);
            return CACHE_ERROR;
        }

        used += (size_t)n;
    }

    buf[used] = '\0';

    close(fd);

    *content = buf;
    *size = used;

    return CACHE_OK;
}

/* ------------------------------------------------------------------ */
/* Add one file to the cache.                                          */
/* ------------------------------------------------------------------ */

static s4 cache_add(struct cache *cache,
                    const struct vfs_entry *entry,
                    const char *url_path)
{
    struct cache_entry *cached;
    char *content;
    size_t size;

    if (cache == NULL ||
        entry == NULL ||
        url_path == NULL)
        return CACHE_ERROR;

    if (entry->type != VFS_FILE)
        return CACHE_ERROR;

    if (read_file(entry->disk_path,
                  &content,
                  &size) != CACHE_OK)
        return CACHE_ERROR;

    cached = (struct cache_entry *)malloc(sizeof(*cached));

    if (cached == NULL) {
        free(content);

        fprintf(stderr,
                "civet: cannot create cache entry: out of memory\n");

        return CACHE_ERROR;
    }

    memset(cached, 0, sizeof(*cached));

    cached->url_path = cache_strdup(url_path);

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
/* Load comma-separated cache paths.                                   */
/* ------------------------------------------------------------------ */

s4 cache_load(struct cache *cache,
              const struct vfs *vfs,
              const char *paths)
{
    char *list;
    char *token;
    char normalized[PATH_MAX];

    const struct vfs_entry *entry;

    if (cache == NULL || vfs == NULL)
        return CACHE_ERROR;

    if (paths == NULL || *paths == '\0')
        return CACHE_OK;

    list = cache_strdup(paths);

    if (list == NULL) {
        fprintf(stderr,
                "civet: cannot parse cache list: out of memory\n");
        return CACHE_ERROR;
    }

    token = strtok(list, ",");

    while (token != NULL) {

        /*
         * Permit whitespace around comma-separated paths.
         */
        while (*token == ' ' ||
               *token == '\t')
            token++;

        {
            char *end;

            end = token + strlen(token);

            while (end > token &&
                   (end[-1] == ' ' ||
                    end[-1] == '\t'))
                end--;

            *end = '\0';
        }

        if (*token == '\0') {
            token = strtok(NULL, ",");
            continue;
        }

        /*
         * Cache paths are virtual paths. Make "index.html" equivalent
         * to "/index.html".
         */
        if (token[0] != '/') {
            char rooted[PATH_MAX];

            if (strlen(token) + 2 > sizeof(rooted)) {
                fprintf(stderr,
                        "civet: cache path too long: '%s'\n",
                        token);
                free(list);
                return CACHE_ERROR;
            }

            rooted[0] = '/';
            strcpy(rooted + 1, token);

            if (vfs_normalize_path(rooted,
                                   normalized,
                                   sizeof(normalized)) != VFS_OK) {
                fprintf(stderr,
                        "civet: invalid cache path: '%s'\n",
                        token);
                free(list);
                return CACHE_ERROR;
            }
        } else {
            if (vfs_normalize_path(token,
                                   normalized,
                                   sizeof(normalized)) != VFS_OK) {
                fprintf(stderr,
                        "civet: invalid cache path: '%s'\n",
                        token);
                free(list);
                return CACHE_ERROR;
            }
        }

        entry = vfs_lookup(vfs,
                           normalized);

        if (entry == NULL) {
            fprintf(stderr,
                    "civet: cache file not found: '%s'\n",
                    normalized);
            free(list);
            return CACHE_ERROR;
        }

        if (entry->type != VFS_FILE) {
            fprintf(stderr,
                    "civet: cache path is not a file: '%s'\n",
                    normalized);
            free(list);
            return CACHE_ERROR;
        }

        /*
         * Don't cache the same file twice.
         */
        if (cache_lookup(cache,
                         normalized) == NULL) {
            if (cache_add(cache,
                          entry,
                          normalized) != CACHE_OK) {
                free(list);
                return CACHE_ERROR;
            }

            fprintf(stderr,
                    "civet: cached %s (%lu bytes)\n",
                    normalized,
                    (unsigned long)cache->entries->size);
        }

        token = strtok(NULL, ",");
    }

    free(list);

    fprintf(stderr,
            "civet: cache contains %lu files (%lu bytes)\n",
            (unsigned long)cache->entry_count,
            (unsigned long)cache->total_bytes);

    return CACHE_OK;
}

/* ------------------------------------------------------------------ */
/* Lookup.                                                              */
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
