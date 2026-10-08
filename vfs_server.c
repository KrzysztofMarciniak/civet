#include "vfs_server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

s4 vfs_server_init(struct vfs_server* server, const char* root) {
        if (server == NULL || root == NULL) return VFS_ERROR;

        server->root_fd = open(root, O_RDONLY);

        if (server->root_fd < 0) {
                fprintf(stderr, "civet: cannot open VFS root '%s': %s\n", root,
                        strerror(errno));
                return VFS_ERROR;
        }

        return VFS_OK;
}

void vfs_server_destroy(struct vfs_server* server) {
        if (server == NULL) return;

        if (server->root_fd >= 0) close(server->root_fd);

        server->root_fd = -1;
}

int vfs_server_open(const struct vfs_server* server,
                    const struct vfs_entry* entry) {
        if (server == NULL || entry == NULL || server->root_fd < 0 ||
            entry->rel_path == NULL)
                return -1;

        if (entry->type != VFS_FILE) return -1;

        return openat(server->root_fd, entry->rel_path,
                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
}
