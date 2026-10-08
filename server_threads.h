#ifndef SERVER_THREADS_H
#define SERVER_THREADS_H

#include "cache.h"
#include "vfs.h"
#include "vfs_server.h"

#define SERVER_MAX_THREADS 64

struct server_thread_args {
        int client_fd;

        struct vfs* vfs;
        struct vfs_server* vfs_server;
        struct cache* cache;
};

s4 server_thread_start(int client_fd, struct vfs* vfs,
                       struct vfs_server* vfs_server, struct cache* cache);

#endif
