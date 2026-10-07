#ifndef VFS_H
#define VFS_H

#include <sys/types.h>
#include <time.h>

#include "lib.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define VFS_FILE      1
#define VFS_DIRECTORY 2

#define VFS_OK         0
#define VFS_NOT_FOUND  1
#define VFS_BAD_PATH  -1
#define VFS_ERROR     -2

struct vfs_entry {
    char *url_path;
    char *disk_path;

    s4 type;

    off_t size;
    time_t mtime;

    struct vfs_entry *next;
};

struct vfs {
    char root[PATH_MAX];

    struct vfs_entry *entries;
    size_t entry_count;
};

s4 vfs_init(struct vfs *vfs, const char *root);

void vfs_destroy(struct vfs *vfs);

/*
 * Normalize an HTTP request path.
 *
 * input:
 *     "/foo/../bar"
 *
 * output:
 *     "/bar"
 *
 * Returns VFS_OK on success.
 */
s4 vfs_normalize_path(const char *input,
                      char *output,
                      size_t output_size);

/*
 * Find a previously indexed file/directory.
 *
 * The path MUST already be normalized.
 */
const struct vfs_entry *
vfs_lookup(const struct vfs *vfs, const char *url_path);
/*
 * Print the virtual filesystem as a tree.
 *
 * The tree represents the URL namespace, not the physical filesystem.
 */
void vfs_dump(const struct vfs *vfs);

#endif
