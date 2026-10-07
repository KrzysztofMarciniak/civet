/* server.h - listening socket and HTTP connection loop.
 *
 * server.c owns the socket, accept loop, connection lifetime, and the
 * transition from received bytes to request_parser.c.
 *
 * Filesystem/path handling deliberately lives in vfs.c.
 */
#ifndef SERVER_H
#define SERVER_H

#include "lib.h"
#include "vfs.h"
#include "cache.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct config {
    const char *bind_addr;       /* IPv4 address to listen on */
    u2          port;            /* TCP port, 1..65535 */

    const char *root_arg;        /* directory as typed */
    char        root[PATH_MAX];  /* canonical absolute document root */

    const char *cache_arg;       /* comma-separated virtual paths */
};

/*
 * Start the server and run until a fatal server error occurs.
 *
 * The VFS and cache are constructed by main() before server_run()
 * is called. server.c uses them but does not own them.
 *
 * Returns 0 only for a clean shutdown.
 * Returns -1 on a runtime error.
 */
s4 server_run(const struct config *cfg,
              struct vfs *vfs,
              struct cache *cache);

#endif
