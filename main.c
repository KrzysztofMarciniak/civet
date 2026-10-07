/* main.c - entry point, and the source of truth for what the server does.
 *
 * Everything the server needs to know is collected into `struct config`
 * here, once, at startup. Other modules receive it, they don't parse
 * arguments or look at the environment themselves.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <unistd.h>

#include "vfs.h"
#include "cache.h"
#include "server.h"
#include "lib.h"
#include "port.h"
#include "allowed_chars.h"

#define PROG_NAME     "civet"
#define PROG_VERSION  "0.1"

#define DEFAULT_BIND  "127.0.0.1"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define EXIT_RUNTIME  1
#define EXIT_USAGE    2

#define PARSE_OK      0
#define PARSE_QUIT    1
#define PARSE_ERR    (-1)

/* ------------------------------------------------------------------ */
/* help                                                                */
/* ------------------------------------------------------------------ */

static void usage(FILE *out)
{
    fputs("Usage: " PROG_NAME " [options] [DIRECTORY]\n"
          "\n"
          "Serve the files in DIRECTORY over HTTP (GET and HEAD only).\n"
          "DIRECTORY defaults to the current directory.\n"
          "\n", out);

    fputs("Options:\n"
          "  -r, --root DIR    directory to serve (same as DIRECTORY)\n"
          "  -p, --port N      TCP port to listen on     (default "
          "8080)\n"
          "  -b, --bind ADDR   IPv4 address to listen on (default "
          DEFAULT_BIND ")\n"
          "                    use 0.0.0.0 to accept connections from "
          "anywhere\n"
          "  -c, --cache PATHS comma-separated files to cache in memory\n"
          "                    e.g. -c index.html,css/style.css\n", out);

    fputs("  -h, --help        show this help and exit\n"
          "  -v, --version     show version and exit\n"
          "\n", out);
}

/* ------------------------------------------------------------------ */
/* argument parsing                                                    */
/* ------------------------------------------------------------------ */

/* Fetch the value that follows option argv[*i]; advances *i. */
static const char *need_arg(int argc, char **argv, s4 *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr,
                PROG_NAME ": option '%s' needs a value\n",
                argv[*i]);
        return NULL;
    }

    *i += 1;

    return argv[*i];
}

static s4 set_root(struct config *cfg, const char *dir)
{
    if (cfg->root_arg != NULL) {
        fprintf(stderr,
                PROG_NAME ": more than one directory given "
                "('%s' and '%s')\n",
                cfg->root_arg,
                dir);
        return -1;
    }

    cfg->root_arg = dir;

    return 0;
}

static s4 parse_args(int argc, char **argv, struct config *cfg)
{
    s4 i;
    s4 opts_done;
    const char *a;
    const char *v;

    opts_done = 0;

    for (i = 1; i < argc; i++) {
        a = argv[i];

        if (opts_done || a[0] != '-' || a[1] == '\0') {
            if (set_root(cfg, a) != 0)
                return PARSE_ERR;
        }
        else if (strcmp(a, "--") == 0) {
            opts_done = 1;
        }
        else if (strcmp(a, "-h") == 0 ||
                 strcmp(a, "--help") == 0) {
            usage(stdout);
            return PARSE_QUIT;
        }
        else if (strcmp(a, "-v") == 0 ||
                 strcmp(a, "--version") == 0) {
            puts(PROG_NAME " " PROG_VERSION);
            return PARSE_QUIT;
        }
        else if (strcmp(a, "-p") == 0 ||
                 strcmp(a, "--port") == 0) {
            v = need_arg(argc, argv, &i);

            if (v == NULL)
                return PARSE_ERR;

            if (parse_port(v, &cfg->port) != 0) {
                fprintf(stderr,
                        PROG_NAME ": invalid port '%s' "
                        "(want 1-65535)\n",
                        v);
                return PARSE_ERR;
            }
        }
        else if (strcmp(a, "-b") == 0 ||
                 strcmp(a, "--bind") == 0) {
            v = need_arg(argc, argv, &i);

            if (v == NULL)
                return PARSE_ERR;

            cfg->bind_addr = v;
        }
        else if (strcmp(a, "-r") == 0 ||
                 strcmp(a, "--root") == 0) {
            v = need_arg(argc, argv, &i);

            if (v == NULL)
                return PARSE_ERR;

            if (set_root(cfg, v) != 0)
                return PARSE_ERR;
        }
        else if (strcmp(a, "-c") == 0 ||
                 strcmp(a, "--cache") == 0) {
            v = need_arg(argc, argv, &i);

            if (v == NULL)
                return PARSE_ERR;

            cfg->cache_arg = v;
        }
        else {
            fprintf(stderr,
                    PROG_NAME ": unknown option '%s'\n",
                    a);
            return PARSE_ERR;
        }
    }

    return PARSE_OK;
}

/* ------------------------------------------------------------------ */
/* document root                                                       */
/* ------------------------------------------------------------------ */

/*
 * Turn the supplied directory into a canonical absolute path.
 *
 * This happens BEFORE the VFS is built. The VFS therefore receives
 * one trusted, canonical filesystem root.
 */
static s4 resolve_root(struct config *cfg)
{
    struct stat st;
    const char *arg;

    arg = (cfg->root_arg != NULL)
        ? cfg->root_arg
        : ".";

    if (realpath(arg, cfg->root) == NULL) {
        fprintf(stderr,
                PROG_NAME ": cannot use '%s': %s\n",
                arg,
                strerror(errno));
        return -1;
    }

    if (stat(cfg->root, &st) != 0 ||
        !S_ISDIR(st.st_mode)) {
        fprintf(stderr,
                PROG_NAME ": '%s' is not a directory\n",
                cfg->root);
        return -1;
    }

    if (access(cfg->root, R_OK | X_OK) != 0) {
        fprintf(stderr,
                PROG_NAME ": '%s' is not readable: %s\n",
                cfg->root,
                strerror(errno));
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    struct config cfg;
    struct vfs vfs;
    struct cache cache;
    s4 r;

    /*
     * Initialize character classification before anything that
     * depends on it.
     */
    ac_init();

    /*
     * Establish configuration defaults.
     */
    cfg.bind_addr = DEFAULT_BIND;
    cfg.port      = DEFAULT_PORT;
    cfg.root_arg  = NULL;
    cfg.cache_arg = NULL;
    cfg.root[0]   = '\0';

    /*
     * Parse command line.
     */
    r = parse_args(argc, argv, &cfg);

    if (r == PARSE_QUIT)
        return 0;

    if (r == PARSE_ERR) {
        fputs("Try '" PROG_NAME
              " --help' for more information.\n",
              stderr);
        return EXIT_USAGE;
    }

    /*
     * Resolve and validate the filesystem root FIRST.
     *
     * vfs_init() depends on cfg.root containing a valid canonical
     * directory path.
     */
    if (resolve_root(&cfg) != 0)
        return EXIT_RUNTIME;

    /*
     * Build the virtual filesystem.
     *
     * From this point onward, request handling should use the VFS
     * rather than constructing filesystem paths from HTTP input.
     */
    if (vfs_init(&vfs, cfg.root) != VFS_OK)
        return EXIT_RUNTIME;

    /*
     * Initialize the in-memory cache.
     *
     * The cache is separate from the VFS: the VFS indexes files,
     * while the cache optionally stores selected file contents.
     */
    cache_init(&cache);

    if (cfg.cache_arg != NULL) {
        if (cache_load(&cache,
                       &vfs,
                       cfg.cache_arg) != CACHE_OK) {
            cache_destroy(&cache);
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

    /*
     * server_run() does not own the VFS or cache.
     * main() owns both for the lifetime of the server.
     */
    r = server_run(&cfg,
                   &vfs,
                   &cache);

    /*
     * Normally server_run() runs forever. If it eventually returns,
     * release the cache and VFS before exiting.
     */
    cache_destroy(&cache);
    vfs_destroy(&vfs);

    if (r != 0)
        return EXIT_RUNTIME;

    return 0;
}
