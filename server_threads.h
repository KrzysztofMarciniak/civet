#ifndef SERVER_THREADS_H
#define SERVER_THREADS_H

#include "cache.h"
#include "vfs.h"
#include "vfs_server.h"

#ifdef ENABLE_TLS
#include "tls_interface.h"
#endif

#define SERVER_MAX_THREADS 64

struct server_thread_args {
        int client_fd;

        struct vfs* vfs;
        struct vfs_server* vfs_server;
        struct cache* cache;

#ifdef ENABLE_TLS
        struct tls_context* tls_ctx;
#endif
};

#ifdef ENABLE_TLS
s4 server_thread_start(int client_fd, struct vfs* vfs,
                       struct vfs_server* vfs_server, struct cache* cache,
                       struct tls_context* tls_ctx);
#else
s4 server_thread_start(int client_fd, struct vfs* vfs,
                       struct vfs_server* vfs_server, struct cache* cache);
#endif

#endif
