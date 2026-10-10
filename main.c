
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "allowed_chars.h"
#include "cache.h"
#include "help.h"
#include "lib.h"
#include "port.h"
#include "server.h"
#include "vfs.h"
#include "vfs_server.h"

#ifdef ENABLE_TLS
#include "tls_interface.h"
#endif

#define PROG_NAME "civet"

#define EXIT_OK 0
#define EXIT_USAGE 2
#define EXIT_RUNTIME 1

static s4 resolve_root(const char* input, char* output, size_t output_size) {
        char resolved[PATH_MAX];
        struct stat st;

        if (realpath(input, resolved) == NULL) {
                fprintf(stderr, PROG_NAME ": cannot resolve root '%s': %s\n",
                        input, strerror(errno));
                return -1;
        }

        if (stat(resolved, &st) < 0) {
                fprintf(stderr, PROG_NAME ": cannot stat root '%s': %s\n",
                        resolved, strerror(errno));
                return -1;
        }

        if (!S_ISDIR(st.st_mode)) {
                fprintf(stderr, PROG_NAME ": root '%s' is not a directory\n",
                        resolved);
                return -1;
        }

        if (access(resolved, R_OK | X_OK) < 0) {
                fprintf(stderr, PROG_NAME ": cannot access root '%s': %s\n",
                        resolved, strerror(errno));
                return -1;
        }

        if (strlen(resolved) + 1 > output_size) {
                fprintf(stderr, PROG_NAME ": root path is too long\n");
                return -1;
        }

        strcpy(output, resolved);

        return 0;
}

int main(int argc, char** argv) {
        struct config cfg;
        struct vfs vfs;
        struct vfs_server vfs_server;
        struct cache cache;
        int opt;
        s4 r;

#ifdef ENABLE_TLS
        const char* cert_file;
        const char* key_file;
        struct tls_config tls_cfg;
        struct tls_context* tls_ctx;
#endif

        cfg.bind_addr = "127.0.0.1";
        cfg.port      = 8080;
        cfg.root_arg  = NULL;
        cfg.root[0]   = '\0';
        cfg.cache_arg = NULL;

#ifdef ENABLE_TLS
        cert_file = NULL;
        key_file  = NULL;
        tls_ctx   = NULL;
#endif

#ifdef ENABLE_TLS
        while ((opt = getopt(argc, argv, "b:p:r:c:C:K:")) != -1) {
#else
        while ((opt = getopt(argc, argv, "b:p:r:c:")) != -1) {
#endif
                switch (opt) {
                        case 'b':
                                cfg.bind_addr = optarg;
                                break;

                        case 'p':
                                if (parse_port(optarg, &cfg.port) != 0) {
                                        fprintf(stderr,
                                                PROG_NAME
                                                ": invalid port '%s'\n",
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

#ifdef ENABLE_TLS
                        case 'C':
                                cert_file = optarg;
                                break;

                        case 'K':
                                key_file = optarg;
                                break;
#endif

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

        if (resolve_root(cfg.root_arg, cfg.root, sizeof(cfg.root)) != 0)
                return EXIT_RUNTIME;

#ifdef ENABLE_TLS
        if (cert_file == NULL || key_file == NULL) {
                fprintf(stderr, PROG_NAME
                        ": TLS build requires -C certificate and -K key\n");
                usage(argv[0]);
                return EXIT_USAGE;
        }

        tls_cfg.certificate_file = cert_file;
        tls_cfg.private_key_file = key_file;
        tls_cfg.backend_config   = NULL;

        if (tls_context_create(&tls_cfg, &tls_ctx) != 0) {
                fprintf(stderr, PROG_NAME ": TLS initialization failed: %s\n",
                        tls_last_error());
                return EXIT_RUNTIME;
        }
#endif

        ac_init();

        if (vfs_init(&vfs, cfg.root) != VFS_OK) {
#ifdef ENABLE_TLS
                tls_context_destroy(tls_ctx);
#endif
                return EXIT_RUNTIME;
        }

        if (vfs_server_init(&vfs_server, cfg.root) != VFS_OK) {
                vfs_destroy(&vfs);
#ifdef ENABLE_TLS
                tls_context_destroy(tls_ctx);
#endif
                return EXIT_RUNTIME;
        }

        cache_init(&cache);

        if (cfg.cache_arg != NULL) {
                if (cache_load(&cache, &vfs, &vfs_server, cfg.cache_arg) !=
                    CACHE_OK) {
                        cache_destroy(&cache);
                        vfs_server_destroy(&vfs_server);
                        vfs_destroy(&vfs);
#ifdef ENABLE_TLS
                        tls_context_destroy(tls_ctx);
#endif
                        return EXIT_RUNTIME;
                }
        }

        vfs_dump(&vfs);

#ifdef ENABLE_TLS
        fprintf(stderr, PROG_NAME ": serving %s on https://%s:%u/\n", cfg.root,
                cfg.bind_addr, (unsigned)cfg.port);
#else
        fprintf(stderr, PROG_NAME ": serving %s on http://%s:%u/\n", cfg.root,
                cfg.bind_addr, (unsigned)cfg.port);
#endif

#ifdef ENABLE_TLS
        r = server_run(&cfg, &vfs, &vfs_server, &cache, tls_ctx);
#else
        r = server_run(&cfg, &vfs, &vfs_server, &cache);
#endif
        cache_destroy(&cache);
        vfs_server_destroy(&vfs_server);
        vfs_destroy(&vfs);

#ifdef ENABLE_TLS
        tls_context_destroy(tls_ctx);
#endif

        return r == 0 ? EXIT_OK : EXIT_RUNTIME;
}
