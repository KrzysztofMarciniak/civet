#ifndef VFS_SERVER_H
#define VFS_SERVER_H

#include "vfs.h"

struct vfs_server {
    int root_fd;
};

s4 vfs_server_init(struct vfs_server *server,
                   const char *root);

void vfs_server_destroy(struct vfs_server *server);

int vfs_server_open(const struct vfs_server *server,
                    const struct vfs_entry *entry);

#endif
