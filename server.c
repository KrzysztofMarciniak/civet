#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "debug.h"
#include "server_threads.h"

#define SERVER_BACKLOG 16
#define EMFILE_BACKOFF_MS 100

static int make_listener(const struct config* cfg) {
        int fd;
        int yes;
        struct sockaddr_in addr;

        debug_log("server", "make_listener: begin");

        fd = socket(AF_INET, SOCK_STREAM, 0);

        if (fd < 0) {
                perror("civet: socket");
                return -1;
        }

        debug_log("server", "make_listener: socket fd=%d", fd);

        yes = 1;

        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0) {
                perror("civet: setsockopt");
                close(fd);
                return -1;
        }

        memset(&addr, 0, sizeof(addr));

        addr.sin_family = AF_INET;
        addr.sin_port   = htons(cfg->port);

        if (inet_pton(AF_INET, cfg->bind_addr, &addr.sin_addr) != 1) {
                fprintf(stderr, "civet: invalid bind address '%s'\n",
                        cfg->bind_addr);
                close(fd);
                return -1;
        }

        debug_log("server", "make_listener: binding %s:%u", cfg->bind_addr,
                  (unsigned)cfg->port);

        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                fprintf(stderr, "civet: bind %s:%u: %s\n", cfg->bind_addr,
                        (unsigned)cfg->port, strerror(errno));
                close(fd);
                return -1;
        }

        debug_log("server", "make_listener: bind successful");

        if (listen(fd, SERVER_BACKLOG) < 0) {
                perror("civet: listen");
                close(fd);
                return -1;
        }

        debug_log("server", "make_listener: listen successful fd=%d", fd);

        return fd;
}

static void sleep_ms(int ms) {
        struct timespec ts;

        ts.tv_sec  = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000;

        nanosleep(&ts, NULL);
}

s4 server_run(const struct config* cfg, struct vfs* vfs,
              struct vfs_server* vfs_server, struct cache* cache) {
        int listener;
        int client_fd;
        s4 rc;

        debug_log("server", "server_run: begin");

        if (cfg == NULL || vfs == NULL || vfs_server == NULL || cache == NULL) {
                debug_log("server", "server_run: invalid argument");
                return -1;
        }

        debug_log("server", "server_run: cfg=%p vfs=%p vfs_server=%p cache=%p",
                  (void*)cfg, (void*)vfs, (void*)vfs_server, (void*)cache);

        listener = make_listener(cfg);

        if (listener < 0) {
                debug_log("server", "server_run: make_listener failed");
                return -1;
        }

        printf("civet: listening on %s:%u\n", cfg->bind_addr,
               (unsigned)cfg->port);

        fflush(stdout);

        debug_log("server", "server_run: entering accept loop");

        for (;;) {
                debug_log("server", "server_run: waiting for accept");

                client_fd = accept(listener, NULL, NULL);

                if (client_fd < 0) {
                        if (errno == EINTR) {
                                debug_log("server",
                                          "server_run: accept interrupted");
                                continue;
                        }

                        if (errno == ECONNABORTED) {
                                debug_log("server",
                                          "server_run: accept ECONNABORTED, "
                                          "continuing");
                                continue;
                        }

                        if (errno == EMFILE || errno == ENFILE) {
                                fprintf(stderr,
                                        "civet: accept %s, backing off\n",
                                        strerror(errno));
                                sleep_ms(EMFILE_BACKOFF_MS);
                                continue;
                        }

                        perror("civet: accept");
                        close(listener);
                        return -1;
                }

                debug_log("server", "server_run: accepted client fd=%d",
                          client_fd);

                debug_log("server", "server_run: starting worker for fd=%d",
                          client_fd);

                rc = server_thread_start(client_fd, vfs, vfs_server, cache);

                debug_log("server",
                          "server_run: server_thread_start returned %d",
                          (int)rc);

                if (rc < 0) {
                        debug_log("server",
                                  "server_run: worker creation failed fd=%d",
                                  client_fd);

                        send(client_fd,
                             "HTTP/1.1 503 Service Unavailable\r\n"
                             "Content-Type: text/plain\r\n"
                             "Content-Length: 13\r\n"
                             "Connection: close\r\n"
                             "\r\n"
                             "server busy\n",
                             sizeof("HTTP/1.1 503 Service Unavailable\r\n"
                                    "Content-Type: text/plain\r\n"
                                    "Content-Length: 13\r\n"
                                    "Connection: close\r\n"
                                    "\r\n"
                                    "server busy\n") -
                                 1,
                             0);

                        close(client_fd);

                        debug_log("server",
                                  "server_run: closed failed client fd=%d",
                                  client_fd);
                }
        }
}
