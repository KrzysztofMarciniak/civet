/* server.h - listening socket and HTTP connection loop.
 *
 * server.c owns the socket, accept loop, connection lifetime, and the
 * transition from received bytes to request_parser.c.
 *
 * Filesystem/path handling lives in vfs.c.
 * Actual filesystem access lives in vfs_server.c.
 */
#ifndef SERVER_H
#define SERVER_H

#include "cache.h"
#include "lib.h"
#include "vfs.h"
#include "vfs_server.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct config {
        const char* bind_addr;
        u2 port;

        const char* root_arg;
        char root[PATH_MAX];

        const char* cache_arg;
};

/*
 * Start the server and run until a fatal server error occurs.
 *
 * main() owns vfs, vfs_server, and cache.
 * server.c only uses them.
 */
s4 server_run(const struct config* cfg, struct vfs* vfs,
              struct vfs_server* vfs_server, struct cache* cache);

#endif
