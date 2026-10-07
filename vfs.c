/* vfs.c - virtual filesystem index for the configured HTTP root. */

#include "vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <sys/stat.h>
#include <dirent.h>

#include <unistd.h>

/* ------------------------------------------------------------------ */
/* internal helpers                                                    */
/* ------------------------------------------------------------------ */

static char *vfs_strdup(const char *s)
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

static s4 add_entry(struct vfs *vfs,
                    const char *url_path,
                    const char *disk_path,
                    const struct stat *st)
{
    struct vfs_entry *entry;

    entry = (struct vfs_entry *)malloc(sizeof(*entry));

    if (entry == NULL)
        return VFS_ERROR;

    memset(entry, 0, sizeof(*entry));

    entry->url_path = vfs_strdup(url_path);

    if (entry->url_path == NULL) {
        free(entry);
        return VFS_ERROR;
    }

    entry->disk_path = vfs_strdup(disk_path);

    if (entry->disk_path == NULL) {
        free(entry->url_path);
        free(entry);
        return VFS_ERROR;
    }

    if (S_ISREG(st->st_mode))
        entry->type = VFS_FILE;
    else if (S_ISDIR(st->st_mode))
        entry->type = VFS_DIRECTORY;
    else {
        free(entry->disk_path);
        free(entry->url_path);
        free(entry);
        return VFS_OK;
    }

    entry->size = st->st_size;
    entry->mtime = st->st_mtime;

    entry->next = vfs->entries;
    vfs->entries = entry;
    vfs->entry_count++;

    return VFS_OK;
}

/*
 * Join two filesystem paths.
 *
 * base must already be a directory path.
 */
static s4 join_disk_path(const char *base,
                         const char *name,
                         char *out,
                         size_t out_size)
{
    size_t base_len;
    size_t name_len;
    size_t needed;

    base_len = strlen(base);
    name_len = strlen(name);

    if (base_len != 0 && base[base_len - 1] == '/')
        needed = base_len + name_len + 1;
    else
        needed = base_len + 1 + name_len + 1;

    if (needed > out_size)
        return -1;

    memcpy(out, base, base_len);

    if (base_len != 0 && base[base_len - 1] == '/') {
        memcpy(out + base_len,
               name,
               name_len + 1);
    } else {
        out[base_len] = '/';

        memcpy(out + base_len + 1,
               name,
               name_len + 1);
    }

    return 0;
}

/*
 * Append one component to a virtual URL path.
 */
static s4 join_url_path(const char *base,
                        const char *name,
                        char *out,
                        size_t out_size)
{
    size_t base_len;
    size_t name_len;
    size_t needed;

    base_len = strlen(base);
    name_len = strlen(name);

    if (strcmp(base, "/") == 0)
        needed = 1 + name_len + 1;
    else
        needed = base_len + 1 + name_len + 1;

    if (needed > out_size)
        return -1;

    if (strcmp(base, "/") == 0) {
        out[0] = '/';

        memcpy(out + 1,
               name,
               name_len + 1);
    } else {
        memcpy(out, base, base_len);

        out[base_len] = '/';

        memcpy(out + base_len + 1,
               name,
               name_len + 1);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* recursive directory scan                                            */
/* ------------------------------------------------------------------ */

static s4 scan_directory(struct vfs *vfs,
                         const char *disk_dir,
                         const char *url_dir)
{
    DIR *dir;
    struct dirent *de;

    char disk_path[PATH_MAX];
    char url_path[PATH_MAX];

    struct stat st;

    dir = opendir(disk_dir);

    if (dir == NULL) {
        fprintf(stderr,
                "civet: cannot read directory '%s': %s\n",
                disk_dir,
                strerror(errno));
        return VFS_ERROR;
    }

    while ((de = readdir(dir)) != NULL) {

        /*
         * Never recurse into "." or "..".
         */
        if (strcmp(de->d_name, ".") == 0 ||
            strcmp(de->d_name, "..") == 0)
            continue;

        /*
         * Construct paths from filesystem names discovered by us,
         * never from HTTP input.
         */
        if (join_disk_path(disk_dir,
                           de->d_name,
                           disk_path,
                           sizeof(disk_path)) != 0) {
            fprintf(stderr,
                    "civet: path too long: '%s/%s'\n",
                    disk_dir,
                    de->d_name);
            closedir(dir);
            return VFS_ERROR;
        }

        if (join_url_path(url_dir,
                          de->d_name,
                          url_path,
                          sizeof(url_path)) != 0) {
            fprintf(stderr,
                    "civet: URL path too long: '%s/%s'\n",
                    url_dir,
                    de->d_name);
            closedir(dir);
            return VFS_ERROR;
        }

        /*
         * Use lstat(), not stat().
         *
         * This means a symlink is identified as a symlink instead of
         * following it somewhere outside our configured root.
         */
        if (lstat(disk_path, &st) != 0) {
            fprintf(stderr,
                    "civet: cannot stat '%s': %s\n",
                    disk_path,
                    strerror(errno));
            continue;
        }

        /*
         * Deliberately ignore symlinks and all special files.
         */
        if (!S_ISREG(st.st_mode) &&
            !S_ISDIR(st.st_mode))
            continue;

        if (add_entry(vfs,
                      url_path,
                      disk_path,
                      &st) != VFS_OK) {
            closedir(dir);
            return VFS_ERROR;
        }

        if (S_ISDIR(st.st_mode)) {
            if (scan_directory(vfs,
                               disk_path,
                               url_path) != VFS_OK) {
                closedir(dir);
                return VFS_ERROR;
            }
        }
    }

    closedir(dir);

    return VFS_OK;
}

/* ------------------------------------------------------------------ */
/* public initialization                                               */
/* ------------------------------------------------------------------ */

s4 vfs_init(struct vfs *vfs, const char *root)
{
    struct stat st;
    char normalized_root[PATH_MAX];

    if (vfs == NULL || root == NULL)
        return VFS_ERROR;

    memset(vfs, 0, sizeof(*vfs));

    /*
     * cfg->root has already gone through realpath() in main.c.
     *
     * Still verify it here because vfs_init() should have a clean
     * contract of its own.
     */
    if (realpath(root,
                 normalized_root) == NULL) {
        fprintf(stderr,
                "civet: cannot resolve VFS root '%s': %s\n",
                root,
                strerror(errno));
        return VFS_ERROR;
    }

    if (stat(normalized_root, &st) != 0) {
        fprintf(stderr,
                "civet: cannot stat VFS root '%s': %s\n",
                normalized_root,
                strerror(errno));
        return VFS_ERROR;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr,
                "civet: VFS root is not a directory: '%s'\n",
                normalized_root);
        return VFS_ERROR;
    }

    if (strlen(normalized_root) >= sizeof(vfs->root)) {
        fprintf(stderr,
                "civet: VFS root path is too long\n");
        return VFS_ERROR;
    }

    strcpy(vfs->root, normalized_root);

    /*
     * The virtual root itself represents the configured root
     * directory.
     */
    if (add_entry(vfs,
                  "/",
                  vfs->root,
                  &st) != VFS_OK) {
        vfs_destroy(vfs);
        return VFS_ERROR;
    }

    if (scan_directory(vfs,
                       vfs->root,
                       "/") != VFS_OK) {
        vfs_destroy(vfs);
        return VFS_ERROR;
    }

    fprintf(stderr,
            "civet: indexed %lu filesystem entries\n",
            (unsigned long)vfs->entry_count);

    return VFS_OK;
}

/* ------------------------------------------------------------------ */
/* destruction                                                         */
/* ------------------------------------------------------------------ */

void vfs_destroy(struct vfs *vfs)
{
    struct vfs_entry *entry;
    struct vfs_entry *next;

    if (vfs == NULL)
        return;

    entry = vfs->entries;

    while (entry != NULL) {
        next = entry->next;

        free(entry->url_path);
        free(entry->disk_path);
        free(entry);

        entry = next;
    }

    vfs->entries = NULL;
    vfs->entry_count = 0;
}

/* ------------------------------------------------------------------ */
/* URL path normalization                                               */
/* ------------------------------------------------------------------ */

static s4 hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return (s4)(c - '0');

    if (c >= 'a' && c <= 'f')
        return (s4)(c - 'a' + 10);

    if (c >= 'A' && c <= 'F')
        return (s4)(c - 'A' + 10);

    return -1;
}

/*
 * Decode the path portion of an HTTP request-target.
 *
 * The query string is not part of the filesystem path.
 *
 * Percent decoding is performed exactly once.
 */
static s4 decode_request_path(const char *input,
                              char *output,
                              size_t output_size)
{
    const char *p;
    size_t out_len;
    s4 hi;
    s4 lo;
    unsigned char value;

    if (input == NULL ||
        output == NULL ||
        output_size == 0)
        return VFS_BAD_PATH;

    if (input[0] != '/')
        return VFS_BAD_PATH;

    p = input;
    out_len = 0;

    while (*p != '\0' && *p != '?') {

        if (out_len + 1 >= output_size)
            return VFS_BAD_PATH;

        if (*p != '%') {
            value = (unsigned char)*p;
            p++;
        } else {
            /*
             * '%' must be followed by exactly two hexadecimal digits.
             */
            if (p[1] == '\0' ||
                p[2] == '\0' ||
                p[1] == '?' ||
                p[2] == '?')
                return VFS_BAD_PATH;

            hi = hex_value(p[1]);
            lo = hex_value(p[2]);

            if (hi < 0 || lo < 0)
                return VFS_BAD_PATH;

            value = (unsigned char)((hi << 4) | lo);

            p += 3;
        }

        /*
         * NUL can never be part of a POSIX pathname and must never
         * reach filesystem functions.
         */
        if (value == 0)
            return VFS_BAD_PATH;

        /*
         * Reject backslash so the virtual path model does not depend
         * on platform-specific path semantics.
         */
        if (value == '\\')
            return VFS_BAD_PATH;

        output[out_len++] = (char)value;
    }

    output[out_len] = '\0';

    return VFS_OK;
}

s4 vfs_normalize_path(const char *input,
                      char *output,
                      size_t output_size)
{
    char decoded[PATH_MAX];

    const char *p;
    const char *component_start;

    size_t out_len;
    size_t component_len;

    /*
     * Decode before checking for "..".
     *
     * This makes all of these equivalent security failures:
     *
     *     /../foo
     *     /%2e%2e/foo
     *     /.%2e/foo
     *     /%2E%2E/foo
     */
    if (decode_request_path(input,
                            decoded,
                            sizeof(decoded)) != VFS_OK)
        return VFS_BAD_PATH;

    if (decoded[0] != '/')
        return VFS_BAD_PATH;

    if (output == NULL ||
        output_size < 2)
        return VFS_BAD_PATH;

    output[0] = '/';
    out_len = 1;

    p = decoded + 1;

    while (*p != '\0') {

        /*
         * Collapse repeated '/' characters.
         */
        while (*p == '/')
            p++;

        if (*p == '\0')
            break;

        component_start = p;

        while (*p != '/' && *p != '\0')
            p++;

        component_len = (size_t)(p - component_start);

        /*
         * "." has no effect.
         */
        if (component_len == 1 &&
            component_start[0] == '.')
            continue;

        /*
         * Reject ".." completely rather than allowing it to modify
         * the virtual path stack.
         */
        if (component_len == 2 &&
            component_start[0] == '.' &&
            component_start[1] == '.')
            return VFS_BAD_PATH;

        /*
         * Add separator before every component except the first.
         */
        if (out_len > 1) {
            if (out_len + 1 >= output_size)
                return VFS_BAD_PATH;

            output[out_len++] = '/';
        }

        if (out_len + component_len >= output_size)
            return VFS_BAD_PATH;

        memcpy(output + out_len,
               component_start,
               component_len);

        out_len += component_len;
    }

    /*
     * Empty normalized path means root.
     */
    if (out_len == 1) {
        output[1] = '\0';
        return VFS_OK;
    }

    output[out_len] = '\0';

    return VFS_OK;
}

/* ------------------------------------------------------------------ */
/* lookup                                                               */
/* ------------------------------------------------------------------ */

const struct vfs_entry *
vfs_lookup(const struct vfs *vfs, const char *url_path)
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
/* virtual filesystem tree                                              */
/* ------------------------------------------------------------------ */

static int vfs_entry_compare(const void *a, const void *b)
{
    const struct vfs_entry *ea;
    const struct vfs_entry *eb;

    ea = *(const struct vfs_entry * const *)a;
    eb = *(const struct vfs_entry * const *)b;

    return strcmp(ea->url_path, eb->url_path);
}

static s4 vfs_path_depth(const char *path)
{
    s4 depth;

    depth = 0;

    while (*path != '\0') {
        if (*path == '/')
            depth++;

        path++;
    }

    if (depth > 0)
        depth--;

    return depth;
}

static void vfs_print_indent(s4 depth)
{
    s4 i;

    for (i = 0; i < depth; i++)
        fputs("    ", stderr);
}

void vfs_dump(const struct vfs *vfs)
{
    struct vfs_entry **items;
    const struct vfs_entry *entry;
    size_t i;
    size_t count;
    s4 depth;
    s4 directories;
    s4 files;

    if (vfs == NULL)
        return;

    if (vfs->entry_count == 0) {
        fprintf(stderr,
                "civet: virtual filesystem is empty\n");
        return;
    }

    items = (struct vfs_entry **)malloc(
        vfs->entry_count * sizeof(*items));

    if (items == NULL) {
        fprintf(stderr,
                "civet: cannot display virtual filesystem: "
                "out of memory\n");
        return;
    }

    count = 0;
    directories = 0;
    files = 0;

    for (entry = vfs->entries;
         entry != NULL;
         entry = entry->next) {

        items[count] = (struct vfs_entry *)entry;
        count++;

        if (entry->type == VFS_DIRECTORY)
            directories++;
        else if (entry->type == VFS_FILE)
            files++;
    }

    qsort(items,
          count,
          sizeof(*items),
          vfs_entry_compare);

    fprintf(stderr,
            "civet: virtual filesystem:\n");

    fprintf(stderr, "/\n");

    for (i = 0; i < count; i++) {
        entry = items[i];

        if (strcmp(entry->url_path, "/") == 0)
            continue;

        depth = vfs_path_depth(entry->url_path);

        vfs_print_indent(depth);

        if (entry->type == VFS_DIRECTORY) {
            fprintf(stderr,
                    "|-- %s/\n",
                    strrchr(entry->url_path, '/') + 1);
        } else if (entry->type == VFS_FILE) {
            fprintf(stderr,
                    "|-- %s\n",
                    strrchr(entry->url_path, '/') + 1);
        }
    }

    fprintf(stderr,
            "civet: %ld directories, %ld files\n",
            (long)directories,
            (long)files);

    free(items);
}
