#include "vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <dirent.h>
#include <unistd.h>

#define VFS_MAX_SCAN_DEPTH 64
#define VFS_HEX "0123456789ABCDEF"

static int is_url_unreserved(unsigned char c)
{
    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9'))
        return 1;

    return c == '-' || c == '.' || c == '_' || c == '~';
}

static size_t url_component_encoded_length(const char *name)
{
    const unsigned char *p;
    size_t n;

    p = (const unsigned char *)name;
    n = 0;

    while (*p != '\0') {
        n += is_url_unreserved(*p) ? 1 : 3;
        p++;
    }

    return n;
}

static s4 url_encode_component(const char *name,
                               char *out,
                               size_t out_size)
{
    const unsigned char *p;
    size_t n;
    unsigned char c;

    if (name == NULL || out == NULL || out_size == 0)
        return VFS_BAD_PATH;

    n = url_component_encoded_length(name);

    if (n >= out_size)
        return VFS_BAD_PATH;

    p = (const unsigned char *)name;
    n = 0;

    while (*p != '\0') {
        c = *p++;

        if (is_url_unreserved(c)) {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = VFS_HEX[(c >> 4) & 0x0F];
            out[n++] = VFS_HEX[c & 0x0F];
        }
    }

    out[n] = '\0';
    return VFS_OK;
}

/* ------------------------------------------------------------------ */
/* entries                                                             */
/* ------------------------------------------------------------------ */

s4 vfs_add(struct vfs *vfs,
           const char *url_path,
           const char *rel_path,
           const struct stat *st)
{
    struct vfs_entry *entry;
    char *p;

    size_t url_len;
    size_t rel_len;
    size_t total;

    if (vfs == NULL ||
        url_path == NULL ||
        rel_path == NULL ||
        st == NULL)
        return VFS_ERROR;

    if (!S_ISREG(st->st_mode) &&
        !S_ISDIR(st->st_mode))
        return VFS_OK;

    url_len = strlen(url_path);
    rel_len = strlen(rel_path);

    total = sizeof(*entry);

    if (url_len > (size_t)-1 - 1 - total)
        return VFS_ERROR;

    total += url_len + 1;

    if (rel_len > (size_t)-1 - 1 - total)
        return VFS_ERROR;

    total += rel_len + 1;

    entry = (struct vfs_entry *)malloc(total);

    if (entry == NULL)
        return VFS_ERROR;

    p = (char *)(entry + 1);

    entry->url_path = p;
    memcpy(p, url_path, url_len + 1);

    p += url_len + 1;

    entry->rel_path = p;
    memcpy(p, rel_path, rel_len + 1);

    entry->type = S_ISDIR(st->st_mode)
                ? VFS_DIRECTORY
                : VFS_FILE;

    entry->size = st->st_size;
    entry->mtime = st->st_mtime;

    entry->next = vfs->entries;
    vfs->entries = entry;
    vfs->entry_count++;

    return VFS_OK;
}

s4 vfs_remove(struct vfs *vfs,
              const char *url_path)
{
    struct vfs_entry **p;
    struct vfs_entry *entry;

    if (vfs == NULL || url_path == NULL)
        return VFS_ERROR;

    p = &vfs->entries;

    while (*p != NULL) {
        entry = *p;

        if (strcmp(entry->url_path, url_path) == 0) {
            *p = entry->next;
            free(entry);

            vfs->entry_count--;

            return VFS_OK;
        }

        p = &entry->next;
    }

    return VFS_NOT_FOUND;
}

/* ------------------------------------------------------------------ */
/* path construction                                                   */
/* ------------------------------------------------------------------ */

static s4 join_url_path(const char *base,
                        const char *name,
                        char *out,
                        size_t out_size)
{
    size_t base_len;
    size_t enc_len;
    size_t out_len;

    if (base == NULL ||
        name == NULL ||
        out == NULL ||
        out_size == 0)
        return VFS_BAD_PATH;

    base_len = strlen(base);
    enc_len = url_component_encoded_length(name);

    if (base_len == 1 && base[0] == '/') {
        if (enc_len + 2 > out_size)
            return VFS_BAD_PATH;

        out[0] = '/';
        out_len = 1;
    } else {
        if (base_len + enc_len + 3 > out_size)
            return VFS_BAD_PATH;

        memcpy(out, base, base_len);
        out_len = base_len;
        out[out_len++] = '/';
    }

    if (url_encode_component(name,
                             out + out_len,
                             out_size - out_len) != VFS_OK)
        return VFS_BAD_PATH;

    return VFS_OK;
}

static s4 join_rel_path(const char *base,
                        const char *name,
                        char *out,
                        size_t out_size)
{
    size_t base_len;
    size_t name_len;

    if (base == NULL ||
        name == NULL ||
        out == NULL ||
        out_size == 0)
        return VFS_BAD_PATH;

    base_len = strlen(base);
    name_len = strlen(name);

    if (base_len == 0) {
        if (name_len + 1 > out_size)
            return VFS_BAD_PATH;

        memcpy(out, name, name_len + 1);
        return VFS_OK;
    }

    if (base_len + name_len + 2 > out_size)
        return VFS_BAD_PATH;

    memcpy(out, base, base_len);
    out[base_len] = '/';
    memcpy(out + base_len + 1, name, name_len + 1);

    return VFS_OK;
}

/* ------------------------------------------------------------------ */
/* scanning                                                            */
/* ------------------------------------------------------------------ */

static s4 scan_directory(struct vfs *vfs,
                         const char *disk_dir,
                         const char *rel_dir,
                         const char *url_dir,
                         s4 depth)
{
    DIR *dir;
    struct dirent *de;

    char disk_path[PATH_MAX];
    char rel_path[PATH_MAX];
    char url_path[PATH_MAX];

    struct stat st;

    if (depth > VFS_MAX_SCAN_DEPTH)
        return VFS_ERROR;

    dir = opendir(disk_dir);

    if (dir == NULL) {
        fprintf(stderr,
                "civet: cannot read '%s': %s\n",
                disk_dir,
                strerror(errno));
        return VFS_ERROR;
    }

    errno = 0;

    while ((de = readdir(dir)) != NULL) {

        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0)
            continue;

        if (join_rel_path(rel_dir,
                          de->d_name,
                          rel_path,
                          sizeof(rel_path)) != VFS_OK)
            continue;

        if (join_url_path(url_dir,
                          de->d_name,
                          url_path,
                          sizeof(url_path)) != VFS_OK)
            continue;

        if (snprintf(disk_path,
                     sizeof(disk_path),
                     "%s/%s",
                     disk_dir,
                     de->d_name) < 0)
            continue;

        if (lstat(disk_path, &st) != 0) {
            if (errno == ENOENT)
                continue;

            fprintf(stderr,
                    "civet: cannot stat '%s': %s\n",
                    disk_path,
                    strerror(errno));
            continue;
        }

        if (!S_ISREG(st.st_mode) &&
            !S_ISDIR(st.st_mode))
            continue;

        if (vfs_add(vfs,
                    url_path,
                    rel_path,
                    &st) != VFS_OK) {
            closedir(dir);
            return VFS_ERROR;
        }

        if (S_ISDIR(st.st_mode)) {
            if (scan_directory(vfs,
                               disk_path,
                               rel_path,
                               url_path,
                               depth + 1) != VFS_OK) {
                closedir(dir);
                return VFS_ERROR;
            }
        }
    }

    if (errno != 0) {
        fprintf(stderr,
                "civet: error reading '%s': %s\n",
                disk_dir,
                strerror(errno));
        closedir(dir);
        return VFS_ERROR;
    }

    closedir(dir);
    return VFS_OK;
}

/* ------------------------------------------------------------------ */
/* initialization                                                      */
/* ------------------------------------------------------------------ */

s4 vfs_init(struct vfs *vfs, const char *root)
{
    struct stat st;
    char real_root[PATH_MAX];

    if (vfs == NULL || root == NULL)
        return VFS_ERROR;

    memset(vfs, 0, sizeof(*vfs));

    if (realpath(root, real_root) == NULL) {
        fprintf(stderr,
                "civet: cannot resolve '%s': %s\n",
                root,
                strerror(errno));
        return VFS_ERROR;
    }

    if (stat(real_root, &st) != 0 ||
        !S_ISDIR(st.st_mode)) {
        fprintf(stderr,
                "civet: VFS root is not a directory: '%s'\n",
                real_root);
        return VFS_ERROR;
    }

    if (strlen(real_root) >= sizeof(vfs->root))
        return VFS_ERROR;

    strcpy(vfs->root, real_root);

    if (vfs_add(vfs, "/", "", &st) != VFS_OK) {
        vfs_destroy(vfs);
        return VFS_ERROR;
    }

    if (scan_directory(vfs,
                       vfs->root,
                       "",
                       "/",
                       0) != VFS_OK) {
        vfs_destroy(vfs);
        return VFS_ERROR;
    }

    fprintf(stderr,
            "civet: indexed %lu entries\n",
            (unsigned long)vfs->entry_count);

    return VFS_OK;
}

void vfs_destroy(struct vfs *vfs)
{
    struct vfs_entry *entry;
    struct vfs_entry *next;

    if (vfs == NULL)
        return;

    entry = vfs->entries;

    while (entry != NULL) {
        next = entry->next;
        free(entry);
        entry = next;
    }

    vfs->entries = NULL;
    vfs->entry_count = 0;
    vfs->root[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* normalization                                                       */
/* ------------------------------------------------------------------ */

static s4 hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';

    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;

    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;

    return -1;
}

static s4 decode_component(const char *input,
                           size_t len,
                           char *out,
                           size_t out_size)
{
    size_t i;
    size_t n;
    s4 hi;
    s4 lo;
    unsigned char c;

    n = 0;

    for (i = 0; i < len; i++) {

        if (input[i] == '%') {
            if (i + 2 >= len)
                return VFS_BAD_PATH;

            hi = hex_value(input[i + 1]);
            lo = hex_value(input[i + 2]);

            if (hi < 0 || lo < 0)
                return VFS_BAD_PATH;

            c = (unsigned char)((hi << 4) | lo);
            i += 2;
        } else {
            c = (unsigned char)input[i];
        }

        if (c == 0 ||
            c == '/' ||
            c == '\\' ||
            c < 0x20 ||
            c == 0x7f)
            return VFS_BAD_PATH;

        if (n + 1 >= out_size)
            return VFS_BAD_PATH;

        out[n++] = (char)c;
    }

    out[n] = '\0';
    return VFS_OK;
}

s4 vfs_normalize_path(const char *input,
                      char *output,
                      size_t output_size)
{
    size_t input_len;
    size_t component_len;
    size_t out_len;

    const char *p;
    const char *start;

    char decoded[PATH_MAX];
    char encoded[PATH_MAX];

    if (input == NULL ||
        output == NULL ||
        output_size < 2 ||
        input[0] != '/')
        return VFS_BAD_PATH;

    input_len = 0;

    while (input[input_len] != '\0' &&
           input[input_len] != '?') {

        if (input_len >= PATH_MAX - 1)
            return VFS_BAD_PATH;

        input_len++;
    }

    output[0] = '/';
    out_len = 1;

    p = input + 1;

    while ((size_t)(p - input) < input_len) {

        if (*p == '/') {
            if (out_len + 1 >= output_size)
                return VFS_BAD_PATH;

            output[out_len++] = '/';
            p++;
            continue;
        }

        start = p;

        while ((size_t)(p - input) < input_len &&
               *p != '/')
            p++;

        component_len = (size_t)(p - start);

        if (decode_component(start,
                             component_len,
                             decoded,
                             sizeof(decoded)) != VFS_OK)
            return VFS_BAD_PATH;

        if (strcmp(decoded, ".") == 0 ||
            strcmp(decoded, "..") == 0)
            return VFS_BAD_PATH;

        if (url_encode_component(decoded,
                                 encoded,
                                 sizeof(encoded)) != VFS_OK)
            return VFS_BAD_PATH;

        component_len = strlen(encoded);

        if (component_len >
            output_size - out_len - 1)
            return VFS_BAD_PATH;

        memcpy(output + out_len,
               encoded,
               component_len);

        out_len += component_len;
    }

    output[out_len] = '\0';

    return VFS_OK;
}

/* ------------------------------------------------------------------ */
/* lookup                                                              */
/* ------------------------------------------------------------------ */

const struct vfs_entry *
vfs_lookup(const struct vfs *vfs,
           const char *url_path)
{
    const struct vfs_entry *entry;

    if (vfs == NULL || url_path == NULL)
        return NULL;

    for (entry = vfs->entries;
         entry != NULL;
         entry = entry->next) {

        if (strcmp(entry->url_path, url_path) == 0)
            return entry;
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* dump                                                                */
/* ------------------------------------------------------------------ */

static int entry_compare(const void *a, const void *b)
{
    const struct vfs_entry *ea;
    const struct vfs_entry *eb;

    ea = *(const struct vfs_entry * const *)a;
    eb = *(const struct vfs_entry * const *)b;

    return strcmp(ea->url_path, eb->url_path);
}

void vfs_dump(const struct vfs *vfs)
{
    const struct vfs_entry *entry;
    struct vfs_entry **items;

    size_t i;
    size_t n;

    if (vfs == NULL || vfs->entry_count == 0)
        return;

    items = (struct vfs_entry **)malloc(
        vfs->entry_count * sizeof(*items));

    if (items == NULL)
        return;

    n = 0;

    for (entry = vfs->entries;
         entry != NULL;
         entry = entry->next)
        items[n++] = (struct vfs_entry *)entry;

    qsort(items,
          n,
          sizeof(*items),
          entry_compare);

    fprintf(stderr, "civet: virtual filesystem:\n");

    for (i = 0; i < n; i++) {
        entry = items[i];

        fprintf(stderr,
                "  %s%s\n",
                entry->url_path,
                entry->type == VFS_DIRECTORY ? "/" : "");
    }

    free(items);
}
