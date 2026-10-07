#include "lib.h"
#include "port.h"
#include "allowed_chars.h"
#include "vfs.h"
#include "vfs_server.h"
#include "cache.h"
#include "server.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PROG_NAME "civet"

#define EXIT_OK      0
#define EXIT_USAGE   2
#define EXIT_RUNTIME 1

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-b address] [-p port] -r root [-c paths]\n"
            "\n"
            "  -b address   bind address (default: 127.0.0.1)\n"
            "  -p port      listen port (default: 8080)\n"
            "  -r root      document root\n"
            "  -c paths     comma-separated cache paths\n",
            prog);
}

static s4 resolve_root(const char *input,
                       char *output,
                       size_t output_size)
{
    char resolved[PATH_MAX];
    struct stat st;

    if (realpath(input, resolved) == NULL) {
        fprintf(stderr,
                PROG_NAME ": cannot resolve root '%s': %s\n",
                input,
                strerror(errno));
        return -1;
    }

    if (stat(resolved, &st) < 0) {
        fprintf(stderr,
                PROG_NAME ": cannot stat root '%s': %s\n",
                resolved,
                strerror(errno));
        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr,
                PROG_NAME ": root '%s' is not a directory\n",
                resolved);
        return -1;
    }

    if (access(resolved, R_OK | X_OK) < 0) {
        fprintf(stderr,
                PROG_NAME ": cannot access root '%s': %s\n",
                resolved,
                strerror(errno));
        return -1;
    }

    if (strlen(resolved) + 1 > output_size) {
        fprintf(stderr,
                PROG_NAME ": root path is too long\n");
        return -1;
    }

    strcpy(output, resolved);

    return 0;
}

int main(int argc, char **argv)
{
    struct config cfg;
    struct vfs vfs;
    struct vfs_server vfs_server;
    struct cache cache;
    int opt;
    s4 r;

    cfg.bind_addr = "127.0.0.1";
    cfg.port = 8080;
    cfg.root_arg = NULL;
    cfg.root[0] = '\0';
    cfg.cache_arg = NULL;

    while ((opt = getopt(argc, argv, "b:p:r:c:")) != -1) {
        switch (opt) {
        case 'b':
            cfg.bind_addr = optarg;
            break;

        case 'p':
            if (parse_port(optarg, &cfg.port) != 0) {
                fprintf(stderr,
                        PROG_NAME ": invalid port '%s'\n",
                        optarg);
                return EXIT_USAGE;
            }
            break;

        case 'r':
            cfg.root_arg = optarg;
            break;

        case 'c':
            cfg.cache_arg = optarg;
            break;

        default:
            usage(argv[0]);
            return EXIT_USAGE;
        }
    }

    if (cfg.root_arg == NULL) {
        usage(argv[0]);
        return EXIT_USAGE;
    }

    if (optind != argc) {
        usage(argv[0]);
        return EXIT_USAGE;
    }

    if (resolve_root(cfg.root_arg,
                     cfg.root,
                     sizeof(cfg.root)) != 0)
        return EXIT_RUNTIME;

    ac_init();

    if (vfs_init(&vfs, cfg.root) != VFS_OK)
        return EXIT_RUNTIME;

    if (vfs_server_init(&vfs_server, cfg.root) != VFS_OK) {
        vfs_destroy(&vfs);
        return EXIT_RUNTIME;
    }

    cache_init(&cache);

    if (cfg.cache_arg != NULL) {
        if (cache_load(&cache,
                       &vfs,
                       &vfs_server,
                       cfg.cache_arg) != CACHE_OK) {
            cache_destroy(&cache);
            vfs_server_destroy(&vfs_server);
            vfs_destroy(&vfs);
            return EXIT_RUNTIME;
        }
    }

    vfs_dump(&vfs);

    fprintf(stderr,
            PROG_NAME ": serving %s on http://%s:%u/\n",
            cfg.root,
            cfg.bind_addr,
            (unsigned)cfg.port);

    r = server_run(&cfg,
                   &vfs,
                   &vfs_server,
                   &cache);

    cache_destroy(&cache);
    vfs_server_destroy(&vfs_server);
    vfs_destroy(&vfs);

    return r == 0 ? EXIT_OK : EXIT_RUNTIME;
}
