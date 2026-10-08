#ifndef VFS_H
#define VFS_H

#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "lib.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define VFS_FILE 1
#define VFS_DIRECTORY 2

#define VFS_OK 0
#define VFS_NOT_FOUND 1
#define VFS_BAD_PATH (-1)
#define VFS_ERROR (-2)

struct vfs_entry {
        char* url_path;
        char* rel_path;

        s4 type;

        off_t size;
        time_t mtime;

        struct vfs_entry* next;
};

struct vfs {
        char root[PATH_MAX];

        struct vfs_entry* entries;
        size_t entry_count;
};

s4 vfs_init(struct vfs* vfs, const char* root);
void vfs_destroy(struct vfs* vfs);

s4 vfs_add(struct vfs* vfs, const char* url_path, const char* rel_path,
           const struct stat* st);

s4 vfs_remove(struct vfs* vfs, const char* url_path);

s4 vfs_normalize_path(const char* input, char* output, size_t output_size);

const struct vfs_entry* vfs_lookup(const struct vfs* vfs, const char* url_path);

void vfs_dump(const struct vfs* vfs);

#endif
