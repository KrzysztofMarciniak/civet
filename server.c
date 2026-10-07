/* server.c - listening socket and HTTP connection loop. */

#include "server.h"
#include "cache.h"
#include "request_parser.h"
#include "vfs.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/stat.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <unistd.h>

#define SERVER_BACKLOG       16
#define SERVER_CLIENT_MAX    64
#define SERVER_REQUEST_BUF   16384
#define SERVER_FILE_BUF      8192

/* ------------------------------------------------------------------ */
/* Send all bytes in a buffer.                                         */
/* ------------------------------------------------------------------ */

static s4 send_all(int fd, const char *buf, size_t len)
{
    size_t sent;
    ssize_t n;

    sent = 0;

    while (sent < len) {
        n = send(fd,
                 buf + sent,
                 len - sent,
                 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (n == 0)
            return -1;

        sent += (size_t)n;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Send a simple HTTP response.                                        */
/* ------------------------------------------------------------------ */

static s4 send_response(int fd,
                        s4 status,
                        const char *reason,
                        const char *body)
{
    char response[1024];
    int len;

    len = snprintf(response,
                   sizeof(response),
                   "HTTP/1.1 %ld %s\r\n"
                   "Content-Type: text/plain\r\n"
                   "Content-Length: %lu\r\n"
                   "Connection: close\r\n"
                   "\r\n"
                   "%s",
                   (long)status,
                   reason,
                   (unsigned long)strlen(body),
                   body);

    if (len < 0 ||
        (size_t)len >= sizeof(response))
        return -1;

    return send_all(fd,
                    response,
                    (size_t)len);
}

/* ------------------------------------------------------------------ */
/* MIME type lookup.                                                    */
/* ------------------------------------------------------------------ */

static const char *mime_type(const char *path)
{
    const char *dot;

    dot = strrchr(path, '.');

    if (dot == NULL)
        return "application/octet-stream";

    if (strcmp(dot, ".html") == 0 ||
        strcmp(dot, ".htm") == 0)
        return "text/html";

    if (strcmp(dot, ".css") == 0)
        return "text/css";

    if (strcmp(dot, ".js") == 0)
        return "application/javascript";

    if (strcmp(dot, ".txt") == 0)
        return "text/plain";

    if (strcmp(dot, ".json") == 0)
        return "application/json";

    if (strcmp(dot, ".xml") == 0)
        return "application/xml";

    if (strcmp(dot, ".jpg") == 0 ||
        strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";

    if (strcmp(dot, ".png") == 0)
        return "image/png";

    if (strcmp(dot, ".gif") == 0)
        return "image/gif";

    if (strcmp(dot, ".svg") == 0)
        return "image/svg+xml";

    if (strcmp(dot, ".ico") == 0)
        return "image/x-icon";

    if (strcmp(dot, ".webp") == 0)
        return "image/webp";

    return "application/octet-stream";
}

/* ------------------------------------------------------------------ */
/* Send file headers.                                                   */
/* ------------------------------------------------------------------ */

static s4 send_file_headers(int fd,
                            const char *path,
                            size_t size)
{
    char response[1024];
    int len;

    len = snprintf(response,
                   sizeof(response),
                   "HTTP/1.1 200 OK\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %lu\r\n"
                   "Connection: close\r\n"
                   "\r\n",
                   mime_type(path),
                   (unsigned long)size);

    if (len < 0 ||
        (size_t)len >= sizeof(response))
        return -1;

    return send_all(fd,
                    response,
                    (size_t)len);
}

/* ------------------------------------------------------------------ */
/* Send cached file.                                                    */
/* ------------------------------------------------------------------ */

static s4 send_cached_file(int fd,
                           const struct cache_entry *cached,
                           s4 head_only)
{
    if (cached == NULL)
        return -1;

    if (send_file_headers(fd,
                          cached->url_path,
                          cached->size) != 0)
        return -1;

    if (head_only)
        return 0;

    return send_all(fd,
                    cached->content,
                    cached->size);
}

/* ------------------------------------------------------------------ */
/* Stream an open file to the client.                                   */
/* ------------------------------------------------------------------ */

static s4 send_file(int fd, const char *path)
{
    int file_fd;
    struct stat st;
    char buf[SERVER_FILE_BUF];
    ssize_t n;

    file_fd = open(path, O_RDONLY);

    if (file_fd < 0) {
        if (errno == ENOENT) {
            send_response(fd,
                          404,
                          "Not Found",
                          "not found\n");
            return 0;
        }

        fprintf(stderr,
                "civet: cannot open '%s': %s\n",
                path,
                strerror(errno));

        send_response(fd,
                      500,
                      "Internal Server Error",
                      "internal server error\n");

        return 0;
    }

    /*
     * The VFS was built at startup, but the filesystem can change
     * afterwards. Check the object we actually opened.
     */
    if (fstat(file_fd, &st) != 0) {
        fprintf(stderr,
                "civet: cannot stat open file '%s': %s\n",
                path,
                strerror(errno));

        close(file_fd);

        send_response(fd,
                      500,
                      "Internal Server Error",
                      "internal server error\n");

        return 0;
    }

    if (!S_ISREG(st.st_mode)) {
        close(file_fd);

        send_response(fd,
                      404,
                      "Not Found",
                      "not found\n");

        return 0;
    }

    if (send_file_headers(fd,
                          path,
                          (size_t)st.st_size) != 0) {
        close(file_fd);
        return -1;
    }

    for (;;) {
        n = read(file_fd,
                 buf,
                 sizeof(buf));

        if (n < 0) {
            if (errno == EINTR)
                continue;

            fprintf(stderr,
                    "civet: read error on '%s': %s\n",
                    path,
                    strerror(errno));

            close(file_fd);
            return -1;
        }

        if (n == 0)
            break;

        if (send_all(fd,
                     buf,
                     (size_t)n) != 0) {
            close(file_fd);
            return -1;
        }
    }

    close(file_fd);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Send HEAD response for a file.                                      */
/* ------------------------------------------------------------------ */

static s4 send_file_head(int fd, const char *path)
{
    int file_fd;
    struct stat st;

    file_fd = open(path, O_RDONLY);

    if (file_fd < 0) {
        if (errno == ENOENT) {
            send_response(fd,
                          404,
                          "Not Found",
                          "not found\n");
            return 0;
        }

        fprintf(stderr,
                "civet: cannot open '%s': %s\n",
                path,
                strerror(errno));

        send_response(fd,
                      500,
                      "Internal Server Error",
                      "internal server error\n");

        return 0;
    }

    if (fstat(file_fd, &st) != 0) {
        fprintf(stderr,
                "civet: cannot stat open file '%s': %s\n",
                path,
                strerror(errno));

        close(file_fd);

        send_response(fd,
                      500,
                      "Internal Server Error",
                      "internal server error\n");

        return 0;
    }

    if (!S_ISREG(st.st_mode)) {
        close(file_fd);

        send_response(fd,
                      404,
                      "Not Found",
                      "not found\n");

        return 0;
    }

    if (send_file_headers(fd,
                          path,
                          (size_t)st.st_size) != 0) {
        close(file_fd);
        return -1;
    }

    close(file_fd);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Handle one parsed HTTP request.                                     */
/* ------------------------------------------------------------------ */

static s4 handle_request(int fd,
                         const struct config *cfg,
                         struct vfs *vfs,
                         struct cache *cache,
                         const struct http_request *req)
{
    char path[PATH_MAX];
    char index_path[PATH_MAX];

    const struct vfs_entry *entry;
    const struct vfs_entry *index_entry;
    const struct cache_entry *cached;

    (void)cfg;

    if (req->method != REQUEST_METHOD_GET &&
        req->method != REQUEST_METHOD_HEAD) {
        send_response(fd,
                      405,
                      "Method Not Allowed",
                      "method not allowed\n");
        return 0;
    }

    /*
     * Turn the raw HTTP request-target into a safe virtual path.
     */
    if (vfs_normalize_path(req->target,
                           path,
                           sizeof(path)) != VFS_OK) {
        send_response(fd,
                      400,
                      "Bad Request",
                      "bad request path\n");
        return 0;
    }

    entry = vfs_lookup(vfs,
                       path);

    if (entry == NULL) {
        send_response(fd,
                      404,
                      "Not Found",
                      "not found\n");
        return 0;
    }

    /*
     * Directory requests serve index.html.
     *
     * "/"              -> "/index.html"
     * "/docs"          -> "/docs/index.html"
     * "/docs/"         -> "/docs/index.html"
     */
    if (entry->type == VFS_DIRECTORY) {

        if (strcmp(path, "/") == 0) {
            strcpy(index_path, "/index.html");
        } else {
            if (strlen(path) + strlen("/index.html") + 1 >
                sizeof(index_path)) {
                send_response(fd,
                              400,
                              "Bad Request",
                              "request path too long\n");
                return 0;
            }

            strcpy(index_path, path);
            strcat(index_path, "/index.html");
        }

        index_entry = vfs_lookup(vfs,
                                 index_path);

        if (index_entry == NULL ||
            index_entry->type != VFS_FILE) {
            send_response(fd,
                          403,
                          "Forbidden",
                          "directory access is not allowed\n");
            return 0;
        }

        /*
         * Check the cache using the resolved index path.
         */
        cached = cache_lookup(cache,
                               index_path);

        if (cached != NULL) {
            return send_cached_file(
                fd,
                cached,
                req->method == REQUEST_METHOD_HEAD);
        }

        if (req->method == REQUEST_METHOD_HEAD)
            return send_file_head(fd,
                                  index_entry->disk_path);

        return send_file(fd,
                         index_entry->disk_path);
    }

    if (entry->type != VFS_FILE) {
        send_response(fd,
                      404,
                      "Not Found",
                      "not found\n");
        return 0;
    }

    /*
     * Cache lookup happens after VFS normalization, so the cache
     * only ever sees canonical virtual paths.
     */
    cached = cache_lookup(cache,
                          path);

    if (cached != NULL) {
        return send_cached_file(
            fd,
            cached,
            req->method == REQUEST_METHOD_HEAD);
    }

    if (req->method == REQUEST_METHOD_HEAD)
        return send_file_head(fd,
                              entry->disk_path);

    return send_file(fd,
                     entry->disk_path);
}

/* ------------------------------------------------------------------ */
/* Receive and parse one request.                                       */
/* ------------------------------------------------------------------ */

static s4 handle_client(int fd,
                        const struct config *cfg,
                        struct vfs *vfs,
                        struct cache *cache)
{
    char buf[SERVER_REQUEST_BUF];
    size_t used;
    size_t consumed;
    ssize_t n;
    s4 result;
    struct http_request req;

    used = 0;

    for (;;) {
        if (used >= sizeof(buf)) {
            send_response(fd,
                          431,
                          "Request Header Fields Too Large",
                          "request too large\n");
            return 0;
        }

        n = recv(fd,
                 buf + used,
                 sizeof(buf) - used,
                 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            return -1;
        }

        if (n == 0)
            return 0;

        used += (size_t)n;

        consumed = 0;

        result = request_parse(buf,
                               used,
                               &req,
                               &consumed);

        if (result == REQUEST_PARSE_INCOMPLETE)
            continue;

        if (result == REQUEST_PARSE_TOO_LARGE) {
            send_response(fd,
                          431,
                          "Request Header Fields Too Large",
                          "request too large\n");
            return 0;
        }

        if (result != REQUEST_PARSE_OK) {
            send_response(fd,
                          400,
                          "Bad Request",
                          "bad request\n");
            return 0;
        }

        /*
         * civet currently handles one request per connection.
         */
        return handle_request(fd,
                              cfg,
                              vfs,
                              cache,
                              &req);
    }
}

/* ------------------------------------------------------------------ */
/* Create listening socket.                                            */
/* ------------------------------------------------------------------ */

static s4 make_listener(const struct config *cfg)
{
    int fd;
    int opt;
    struct sockaddr_in addr;

    fd = socket(AF_INET,
                SOCK_STREAM,
                0);

    if (fd < 0) {
        perror("socket");
        return -1;
    }

    opt = 1;

    if (setsockopt(fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &opt,
                   sizeof(opt)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    memset(&addr,
           0,
           sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->port);

    if (inet_pton(AF_INET,
                  cfg->bind_addr,
                  &addr.sin_addr) != 1) {
        fprintf(stderr,
                "civet: invalid bind address: %s\n",
                cfg->bind_addr);
        close(fd);
        return -1;
    }

    if (bind(fd,
             (struct sockaddr *)&addr,
             sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd,
               SERVER_BACKLOG) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

/* ------------------------------------------------------------------ */
/* Close all connected clients.                                        */
/* ------------------------------------------------------------------ */

static void close_clients(int *clients, s4 count)
{
    s4 i;

    for (i = 0; i < count; i++) {
        if (clients[i] >= 0) {
            close(clients[i]);
            clients[i] = -1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Main server loop.                                                    */
/* ------------------------------------------------------------------ */

s4 server_run(const struct config *cfg,
              struct vfs *vfs,
              struct cache *cache)
{
    int listener;
    int clients[SERVER_CLIENT_MAX];

    fd_set readfds;

    s4 client_count;
    s4 i;
    s4 slot;

    int maxfd;
    int ready;

    int client_fd;

    struct sockaddr_in peer;
    socklen_t peer_len;

    listener = make_listener(cfg);

    if (listener < 0)
        return -1;

    for (i = 0; i < SERVER_CLIENT_MAX; i++)
        clients[i] = -1;

    client_count = 0;

    printf("civet: listening on %s:%u\n",
           cfg->bind_addr,
           (unsigned int)cfg->port);

    printf("civet: serving %s\n",
           cfg->root);

    for (;;) {
        FD_ZERO(&readfds);

        FD_SET(listener,
               &readfds);

        maxfd = listener;

        for (i = 0; i < client_count; i++) {
            if (clients[i] >= 0) {
                FD_SET(clients[i],
                       &readfds);

                if (clients[i] > maxfd)
                    maxfd = clients[i];
            }
        }

        ready = select(maxfd + 1,
                       &readfds,
                       NULL,
                       NULL,
                       NULL);

        if (ready < 0) {
            if (errno == EINTR)
                continue;

            perror("select");

            close(listener);
            close_clients(clients,
                          client_count);

            return -1;
        }

        /* ---------------------------------------------------------- */
        /* New connection.                                            */
        /* ---------------------------------------------------------- */

        if (FD_ISSET(listener,
                     &readfds)) {
            peer_len = sizeof(peer);

            client_fd = accept(listener,
                               (struct sockaddr *)&peer,
                               &peer_len);

            if (client_fd < 0) {
                if (errno == EINTR)
                    continue;

                perror("accept");
            } else {
                /*
                 * Reuse any free client slot.
                 */
                slot = -1;

                for (i = 0; i < SERVER_CLIENT_MAX; i++) {
                    if (clients[i] < 0) {
                        slot = i;
                        break;
                    }
                }

                if (slot < 0) {
                    send_response(client_fd,
                                  503,
                                  "Service Unavailable",
                                  "server busy\n");

                    close(client_fd);
                } else {
                    clients[slot] = client_fd;

                    if (slot >= client_count)
                        client_count = slot + 1;
                }
            }
        }

        /* ---------------------------------------------------------- */
        /* Existing clients.                                          */
        /* ---------------------------------------------------------- */

        for (i = 0; i < client_count; i++) {
            if (clients[i] < 0)
                continue;

            if (!FD_ISSET(clients[i],
                          &readfds))
                continue;

            client_fd = clients[i];

            /*
             * One request is handled per connection.
             */
            if (handle_client(client_fd,
                              cfg,
                              vfs,
                              cache) < 0) {
                /*
                 * A single client failure does not stop the server.
                 */
            }

            close(client_fd);
            clients[i] = -1;
        }

        /*
         * Trim unused slots at the end so select() only considers
         * the portion of the array that has been used.
         */
        while (client_count > 0 &&
               clients[client_count - 1] < 0)
            client_count--;
    }

    /* Currently unreachable. */
    close(listener);
    close_clients(clients,
                   client_count);

    return 0;
}
