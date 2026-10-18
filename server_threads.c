#include "server_threads.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "cgi-bin.h"
#include "debug.h"
#include "lib.h"
#include "request_parser.h"
#include "send.h"

#ifdef ENABLE_TLS
#include "tls_interface.h"
#endif

#define SERVER_REQUEST_BUF 16384
#define SERVER_FILE_BUF 8192
#define SERVER_SOCKET_TIMEOUT_SECS 10
#define SERVER_REQUEST_TIMEOUT_SECS 10

static pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static s4 thread_count              = 0;

#ifdef ENABLE_TLS
/*
 * Store the TLS connection in pthread-specific storage so the existing
 * request handlers can use TLS-aware read helpers without sharing
 * connection state between worker threads.
 */
static pthread_key_t tls_connection_key;
static pthread_once_t tls_connection_key_once = PTHREAD_ONCE_INIT;
static int tls_connection_key_status          = -1;

static void create_tls_connection_key(void) {
        tls_connection_key_status =
            pthread_key_create(&tls_connection_key, NULL);
}

static struct tls_connection* current_tls_connection(void) {
        int rc;

        rc = pthread_once(&tls_connection_key_once, create_tls_connection_key);
        if (rc != 0 || tls_connection_key_status != 0) return NULL;

        return (struct tls_connection*)pthread_getspecific(tls_connection_key);
}
#endif

/*
 * Read from the client connection. TLS builds must never silently fall back
 * to plaintext socket I/O.
 */
static ssize_t client_recv(int fd, void* buf, size_t len, int flags) {
#ifdef ENABLE_TLS
        struct tls_connection* conn;

        (void)fd;
        (void)flags;

        conn = current_tls_connection();
        if (conn == NULL) {
                errno = ENOTCONN;
                return -1;
        }

        return tls_connection_read(conn, buf, len);
#else
        return recv(fd, buf, len, flags);
#endif
}

/*
 * Compatibility adapter for the existing request handlers.
 *
 * All outgoing data is sent through client_send_all() from send.h.
 * In TLS builds, the transport uses the current worker's TLS connection.
 * In non-TLS builds, the transport uses the plain socket descriptor.
 */
static s4 send_all(int fd, const char* buf, size_t len) {
        struct client_transport transport;

        transport.fd = fd;

#ifdef ENABLE_TLS
        transport.tls_conn = current_tls_connection();

        if (transport.tls_conn == NULL) {
                errno = ENOTCONN;
                debug_log("thread", "send_all: no TLS connection for fd=%d",
                          fd);
                return -1;
        }
#endif

        debug_log("thread", "send_all: ENTER");
        debug_log("thread", "send_all: fd=%d len=%lu", fd, (unsigned long)len);

        if (client_send_all(&transport, buf, len) < 0) {
                debug_log("thread",
                          "send_all: client_send_all failed: fd=%d "
                          "errno=%d (%s), len=%lu",
                          fd, errno, strerror(errno), (unsigned long)len);
                return -1;
        }

        debug_log("thread", "send_all: EXIT fd=%d sent=%lu", fd,
                  (unsigned long)len);

        return 0;
}

static s4 set_socket_timeouts(int fd) {
        struct timeval tv;

        debug_log("thread", "set_socket_timeouts: ENTER");
        debug_log("thread", "set_socket_timeouts: fd=%d", fd);

        tv.tv_sec  = SERVER_SOCKET_TIMEOUT_SECS;
        tv.tv_usec = 0;

        debug_log("thread", "set_socket_timeouts: setting SO_RCVTIMEO");

        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
                debug_log("thread",
                          "set_socket_timeouts: SO_RCVTIMEO failed: errno=%d "
                          "(%s)",
                          errno, strerror(errno));
                return -1;
        }

        debug_log("thread", "set_socket_timeouts: SO_RCVTIMEO OK");
        debug_log("thread", "set_socket_timeouts: setting SO_SNDTIMEO");

        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
                debug_log("thread",
                          "set_socket_timeouts: SO_SNDTIMEO failed: errno=%d "
                          "(%s)",
                          errno, strerror(errno));
                return -1;
        }

        debug_log("thread", "set_socket_timeouts: SO_SNDTIMEO OK");
        debug_log("thread", "set_socket_timeouts: EXIT");

        return 0;
}

static s4 send_response(int fd, int status, const char* reason,
                        const char* type, const char* body) {
        char buf[1024];
        int n;
        size_t len;

        debug_log("thread", "send_response: ENTER");
        debug_log("thread",
                  "send_response: fd=%d status=%d reason='%s' type='%s'", fd,
                  status, reason != NULL ? reason : "(null)",
                  type != NULL ? type : "(null)");

        if (body == NULL) {
                debug_log("thread", "send_response: body is NULL");
                return -1;
        }

        len = strlen(body);

        debug_log("thread", "send_response: body_len=%lu", (unsigned long)len);

        n = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, reason, type, (unsigned long)len);

        if (n < 0) {
                debug_log("thread",
                          "send_response: snprintf failed: errno=%d (%s)",
                          errno, strerror(errno));
                return -1;
        }

        if ((size_t)n >= sizeof(buf)) {
                debug_log("thread",
                          "send_response: response headers too large: "
                          "n=%d buffer=%lu",
                          n, (unsigned long)sizeof(buf));
                return -1;
        }

        debug_log("thread", "send_response: sending headers len=%d", n);

        if (send_all(fd, buf, (size_t)n) < 0) {
                debug_log("thread", "send_response: sending headers FAILED");
                return -1;
        }

        debug_log("thread", "send_response: headers sent");

        if (send_all(fd, body, len) < 0) {
                debug_log("thread", "send_response: sending body FAILED");
                return -1;
        }

        debug_log("thread", "send_response: body sent");
        debug_log("thread", "send_response: EXIT");

        return 0;
}

static const char* mime_type(const char* path) {
        const char* dot;

        debug_log("thread", "mime_type: path='%s'",
                  path != NULL ? path : "(null)");

        dot = strrchr(path, '.');

        if (dot == NULL) return "application/octet-stream";

        if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
                return "text/html";
        if (strcmp(dot, ".css") == 0) return "text/css";
        if (strcmp(dot, ".js") == 0) return "application/javascript";
        if (strcmp(dot, ".json") == 0) return "application/json";
        if (strcmp(dot, ".txt") == 0) return "text/plain";
        if (strcmp(dot, ".xml") == 0) return "application/xml";
        if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
        if (strcmp(dot, ".png") == 0) return "image/png";
        if (strcmp(dot, ".gif") == 0) return "image/gif";
        if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
                return "image/jpeg";
        if (strcmp(dot, ".webp") == 0) return "image/webp";
        if (strcmp(dot, ".ico") == 0) return "image/x-icon";
        if (strcmp(dot, ".pdf") == 0) return "application/pdf";

        return "application/octet-stream";
}

static s4 send_file_headers(int fd, const char* path, off_t size) {
        char buf[1024];
        int n;

        debug_log("thread", "send_file_headers: ENTER");
        debug_log("thread", "send_file_headers: fd=%d path='%s' size=%lu", fd,
                  path != NULL ? path : "(null)", (unsigned long)size);

        n = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     mime_type(path), (unsigned long)size);

        if (n < 0) {
                debug_log("thread",
                          "send_file_headers: snprintf failed: errno=%d (%s)",
                          errno, strerror(errno));
                return -1;
        }

        if ((size_t)n >= sizeof(buf)) {
                debug_log("thread", "send_file_headers: headers too large n=%d",
                          n);
                return -1;
        }

        debug_log("thread", "send_file_headers: sending %d bytes", n);

        if (send_all(fd, buf, (size_t)n) < 0) {
                debug_log("thread", "send_file_headers: send_all FAILED");
                return -1;
        }

        debug_log("thread", "send_file_headers: EXIT");

        return 0;
}

static s4 send_cached_file(int fd, const struct cache_entry* entry,
                           s4 head_only) {
        char buf[1024];
        int n;

        debug_log("thread", "send_cached_file: ENTER");
        debug_log(
            "thread", "send_cached_file: fd=%d url='%s' size=%lu head=%d", fd,
            entry != NULL && entry->url_path != NULL ? entry->url_path
                                                     : "(null)",
            entry != NULL ? (unsigned long)entry->size : 0UL, (int)head_only);

        n = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     mime_type(entry->url_path), (unsigned long)entry->size);

        if (n < 0) {
                debug_log("thread",
                          "send_cached_file: snprintf failed: errno=%d (%s)",
                          errno, strerror(errno));
                return -1;
        }

        if ((size_t)n >= sizeof(buf)) {
                debug_log("thread", "send_cached_file: headers too large n=%d",
                          n);
                return -1;
        }

        debug_log("thread", "send_cached_file: sending headers len=%d", n);

        if (send_all(fd, buf, (size_t)n) < 0) {
                debug_log("thread", "send_cached_file: header send FAILED");
                return -1;
        }

        if (head_only) {
                debug_log("thread",
                          "send_cached_file: HEAD request, body skipped");
                return 0;
        }

        debug_log("thread", "send_cached_file: sending body len=%lu",
                  (unsigned long)entry->size);

        if (send_all(fd, entry->content, entry->size) < 0) {
                debug_log("thread", "send_cached_file: body send FAILED");
                return -1;
        }

        debug_log("thread", "send_cached_file: EXIT");

        return 0;
}

static s4 send_file(int fd, int file_fd, const char* url_path, s4 head_only) {
        struct stat st;
        char buf[SERVER_FILE_BUF];
        ssize_t n;
#ifdef LIB_HAVE_64
        u8 total_sent;
#else
        unsigned long total_sent;
#endif

        total_sent = 0;

        debug_log("thread", "send_file: ENTER");
        debug_log("thread", "send_file: fd=%d file_fd=%d url='%s' head=%d", fd,
                  file_fd, url_path != NULL ? url_path : "(null)",
                  (int)head_only);

        if (fstat(file_fd, &st) < 0) {
                debug_log("thread", "send_file: fstat failed: errno=%d (%s)",
                          errno, strerror(errno));
                close(file_fd);
                return -1;
        }

        debug_log("thread", "send_file: mode=%o size=%lu",
                  (unsigned)(st.st_mode & 07777), (unsigned long)st.st_size);

        if (!S_ISREG(st.st_mode)) {
                debug_log("thread", "send_file: not regular file");
                close(file_fd);
                return -1;
        }

        if (send_file_headers(fd, url_path, st.st_size) < 0) {
                debug_log("thread", "send_file: headers failed");
                close(file_fd);
                return -1;
        }

        if (head_only) {
                debug_log("thread", "send_file: HEAD request, body skipped");
                close(file_fd);
                return 0;
        }

        for (;;) {
                n = read(file_fd, buf, sizeof(buf));

                if (n < 0) {
                        if (errno == EINTR) {
                                debug_log("thread",
                                          "send_file: read interrupted, retry");
                                continue;
                        }

                        debug_log("thread",
                                  "send_file: read failed: errno=%d (%s)",
                                  errno, strerror(errno));
                        close(file_fd);
                        return -1;
                }

                if (n == 0) break;

                debug_log("thread", "send_file: read %ld bytes", (long)n);

                if (send_all(fd, buf, (size_t)n) < 0) {
                        debug_log("thread",
                                  "send_file: send failed after %lu bytes",
                                  total_sent);
                        close(file_fd);
                        return -1;
                }

                total_sent += (unsigned long)n;
        }

        close(file_fd);

        debug_log("thread", "send_file: EXIT total_sent=%lu", total_sent);

        return 0;
}

static const struct vfs_entry* find_index(const struct vfs* vfs,
                                          const char* path) {
        char index_path[REQUEST_MAX_TARGET];
        size_t len;
        const struct vfs_entry* entry;

        debug_log("thread", "find_index: ENTER");
        debug_log("thread", "find_index: path='%s'",
                  path != NULL ? path : "(null)");

        if (strcmp(path, "/") == 0) {
                debug_log("thread",
                          "find_index: root -> looking up /index.html");

                entry = vfs_lookup(vfs, "/index.html");

                debug_log("thread", "find_index: root lookup complete entry=%p",
                          (void*)entry);

                if (entry != NULL && entry->type == VFS_FILE) {
                        debug_log("thread", "find_index: /index.html FOUND");
                        return entry;
                }

                debug_log("thread", "find_index: /index.html NOT FOUND");
                return NULL;
        }

        len = strlen(path);

        debug_log("thread", "find_index: path_len=%lu", (unsigned long)len);

        if (len + sizeof("/index.html") > sizeof(index_path)) {
                debug_log("thread", "find_index: index path would overflow");
                return NULL;
        }

        memcpy(index_path, path, len);

        if (index_path[len - 1] != '/') index_path[len++] = '/';

        memcpy(index_path + len, "index.html", sizeof("index.html"));

        debug_log("thread", "find_index: lookup='%s'", index_path);

        entry = vfs_lookup(vfs, index_path);

        debug_log("thread", "find_index: lookup complete entry=%p",
                  (void*)entry);

        if (entry != NULL && entry->type == VFS_FILE) {
                debug_log("thread", "find_index: FOUND '%s'", index_path);
                return entry;
        }

        debug_log("thread", "find_index: NOT FOUND '%s'", index_path);

        return NULL;
}

static const char* method_name(s4 method) {
        switch (method) {
                case REQUEST_METHOD_HEAD:
                        return "HEAD";
                case REQUEST_METHOD_POST:
                        return "POST";
                case REQUEST_METHOD_PUT:
                        return "PUT";
                case REQUEST_METHOD_PATCH:
                        return "PATCH";
                case REQUEST_METHOD_DELETE:
                        return "DELETE";
                default:
                        return "GET";
        }
}

static s4 send_405(int fd) {
        static const char msg[] =
            "HTTP/1.1 405 Method Not Allowed\r\n"
            "Allow: GET, HEAD\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 19\r\n"
            "Connection: close\r\n"
            "\r\n"
            "method not allowed\n";

        debug_log("thread", "send_405: fd=%d", fd);

        if (send_all(fd, msg, sizeof(msg) - 1) < 0) {
                debug_log("thread", "send_405: send FAILED");
                return -1;
        }

        debug_log("thread", "send_405: EXIT");

        return 0;
}

/*
 * Collect a CGI request body. Uses client_recv()/send_all(), so it is
 * TLS-aware.
 */
static s4 read_body(int fd, const struct http_request* req, const char* pre,
                    size_t pre_len, char** out, size_t* out_len) {
        const struct request_header* h;
        char* body;
        size_t want;
        size_t got;
        ssize_t n;
        time_t deadline;

        debug_log("thread", "read_body: ENTER");

        *out     = NULL;
        *out_len = 0;

        want = (size_t)req->content_length;

        debug_log("thread", "read_body: fd=%d want=%lu pre_len=%lu", fd,
                  (unsigned long)want, (unsigned long)pre_len);

        if (want == 0) {
                debug_log("thread", "read_body: empty body");
                return 0;
        }

        body = (char*)malloc(want);

        if (body == NULL) {
                debug_log("thread",
                          "read_body: malloc(%lu) failed: errno=%d (%s)",
                          (unsigned long)want, errno, strerror(errno));
                return -1;
        }

        got = pre_len < want ? pre_len : want;

        if (got > 0) {
                memcpy(body, pre, got);
                debug_log("thread",
                          "read_body: copied %lu bytes from pre-buffer",
                          (unsigned long)got);
        }

        if (got < want) {
                h = request_header_get(req, "Expect");

                if (h != NULL) {
                        debug_log("thread",
                                  "read_body: Expect header present len=%lu",
                                  (unsigned long)h->value_len);
                } else {
                        debug_log("thread", "read_body: no Expect header");
                }

                if (h != NULL && h->value_len == 12 &&
                    strncasecmp(h->value, "100-continue", 12) == 0) {
                        debug_log("thread", "read_body: sending 100 Continue");

                        if (send_all(fd, "HTTP/1.1 100 Continue\r\n\r\n",
                                     sizeof("HTTP/1.1 100 Continue\r\n\r\n") -
                                         1) < 0) {
                                debug_log("thread",
                                          "read_body: failed to send "
                                          "100 Continue");
                                free(body);
                                return -1;
                        }
                }
        }

        deadline = time(NULL) + SERVER_REQUEST_TIMEOUT_SECS;

        debug_log("thread", "read_body: deadline=%ld", (long)deadline);

        while (got < want) {
                if (time(NULL) >= deadline) {
                        debug_log("thread",
                                  "read_body: DEADLINE EXPIRED got=%lu "
                                  "want=%lu",
                                  (unsigned long)got, (unsigned long)want);
                        free(body);
                        return -1;
                }

                debug_log("thread", "read_body: BEFORE recv got=%lu want=%lu",
                          (unsigned long)got, (unsigned long)want);

                n = client_recv(fd, body + got, want - got, 0);

                debug_log("thread", "read_body: AFTER recv n=%ld errno=%d",
                          (long)n, errno);

                if (n < 0) {
                        if (errno == EINTR) {
                                debug_log("thread",
                                          "read_body: recv interrupted");
                                continue;
                        }

                        debug_log("thread",
                                  "read_body: recv failed: errno=%d (%s)",
                                  errno, strerror(errno));

                        free(body);
                        return -1;
                }

                if (n == 0) {
                        debug_log("thread",
                                  "read_body: peer closed before full body "
                                  "got=%lu want=%lu",
                                  (unsigned long)got, (unsigned long)want);
                        free(body);
                        return -1;
                }

                got += (size_t)n;

                debug_log("thread", "read_body: received=%ld total=%lu/%lu",
                          (long)n, (unsigned long)got, (unsigned long)want);
        }

        *out     = body;
        *out_len = want;

        debug_log("thread", "read_body: SUCCESS body_len=%lu",
                  (unsigned long)want);

        return 0;
}

static s4 handle_cgi(int fd, const struct vfs* vfs, const struct vfs_entry* ce,
                     const struct http_request* req, const char* pre,
                     size_t pre_len) {
        struct cgi_request creq;
        struct client_transport transport;
        struct sockaddr_in peer;
        struct sockaddr_in local;
        const struct request_header* h;
        socklen_t plen;
        socklen_t llen;
        char remote[INET_ADDRSTRLEN];
        char server[INET_ADDRSTRLEN];
        char port[8];
        char ctype[512];
        char* body;
        size_t body_len;
        size_t n;
        const char* q;
        s4 rc;

        debug_log("thread", "handle_cgi: ENTER");
        debug_log("thread", "handle_cgi: fd=%d ce=%p pre_len=%lu", fd,
                  (void*)ce, (unsigned long)pre_len);
        debug_log("thread", "handle_cgi: target='%s'", req->target);
        debug_log("thread", "handle_cgi: method=%s content_length=%lu",
                  method_name(req->method), (unsigned long)req->content_length);

        if (req->content_length > (u8)CGI_MAX_BODY) {
                debug_log("thread", "handle_cgi: body too large: %lu > %lu",
                          (unsigned long)req->content_length,
                          (unsigned long)CGI_MAX_BODY);

                return send_response(fd, 413, "Payload Too Large", "text/plain",
                                     "body too large\n");
        }

        debug_log("thread", "handle_cgi: BEFORE read_body");

        if (read_body(fd, req, pre, pre_len, &body, &body_len) < 0) {
                debug_log("thread", "handle_cgi: read_body FAILED");

                return send_response(fd, 400, "Bad Request", "text/plain",
                                     "bad request\n");
        }

        debug_log("thread", "handle_cgi: AFTER read_body body_len=%lu",
                  (unsigned long)body_len);

        strcpy(remote, "0.0.0.0");
        strcpy(server, "127.0.0.1");
        strcpy(port, "0");

        plen = sizeof(peer);
        llen = sizeof(local);

        debug_log("thread", "handle_cgi: BEFORE getpeername");

        if (getpeername(fd, (struct sockaddr*)&peer, &plen) == 0) {
                if (inet_ntop(AF_INET, &peer.sin_addr, remote,
                              sizeof(remote)) == NULL) {
                        debug_log("thread",
                                  "handle_cgi: inet_ntop(peer) failed: "
                                  "errno=%d (%s)",
                                  errno, strerror(errno));
                }
        } else {
                debug_log("thread",
                          "handle_cgi: getpeername failed: errno=%d (%s)",
                          errno, strerror(errno));
        }

        debug_log("thread", "handle_cgi: remote='%s'", remote);

        debug_log("thread", "handle_cgi: BEFORE getsockname");

        if (getsockname(fd, (struct sockaddr*)&local, &llen) == 0) {
                if (inet_ntop(AF_INET, &local.sin_addr, server,
                              sizeof(server)) == NULL) {
                        debug_log("thread",
                                  "handle_cgi: inet_ntop(local) failed: "
                                  "errno=%d (%s)",
                                  errno, strerror(errno));
                }

                sprintf(port, "%u", (unsigned)ntohs(local.sin_port));
        } else {
                debug_log("thread",
                          "handle_cgi: getsockname failed: errno=%d (%s)",
                          errno, strerror(errno));
        }

        debug_log("thread", "handle_cgi: server='%s' port='%s'", server, port);

        ctype[0] = '\0';
        h        = request_header_get(req, "Content-Type");

        if (h != NULL) {
                n = h->value_len < sizeof(ctype) - 1 ? h->value_len
                                                     : sizeof(ctype) - 1;

                memcpy(ctype, h->value, n);
                ctype[n] = '\0';

                debug_log("thread", "handle_cgi: content_type='%s'", ctype);
        } else {
                debug_log("thread", "handle_cgi: no Content-Type header");
        }

        q = strchr(req->target, '?');

        if (q != NULL) {
                debug_log("thread", "handle_cgi: query='%s'", q + 1);
        } else {
                debug_log("thread", "handle_cgi: no query string");
        }

        memset(&creq, 0, sizeof(creq));

        creq.method       = method_name(req->method);
        creq.query        = q != NULL ? q + 1 : "";
        creq.content_type = ctype;
        creq.remote_addr  = remote;
        creq.server_name  = server;
        creq.server_port  = port;
        creq.body         = body;
        creq.body_len     = body_len;
        creq.headers      = req->headers;
        creq.header_count = (size_t)req->header_count;

        memset(&transport, 0, sizeof(transport));
        transport.fd = fd;

#ifdef ENABLE_TLS
        transport.tls_conn = current_tls_connection();

        if (transport.tls_conn == NULL) {
                debug_log("thread", "handle_cgi: no TLS connection");
                free(body);
                return -1;
        }
#endif

        debug_log("thread", "handle_cgi: CGI request prepared");
        debug_log("thread", "handle_cgi: BEFORE cgi_run");

        rc = cgi_run(vfs, ce, &creq, &transport);

        debug_log("thread", "handle_cgi: AFTER cgi_run rc=%d", (int)rc);

        free(body);

        debug_log("thread", "handle_cgi: EXIT rc=%d", (int)rc);

        return rc;
}

static s4 handle_request(int fd, struct vfs* vfs, struct vfs_server* vfs_server,
                         struct cache* cache, const struct http_request* req,
                         const char* pre, size_t pre_len) {
        char path[REQUEST_MAX_TARGET];
        size_t len;
        const struct vfs_entry* entry;
        const struct cache_entry* cached;
        int file_fd;

        debug_log("thread", "handle_request: ENTER");
        debug_log("thread", "handle_request: fd=%d target='%s'", fd,
                  req->target);
        debug_log("thread", "handle_request: method=%s",
                  method_name(req->method));
        debug_log("thread", "handle_request: content_length=%lu",
                  (unsigned long)req->content_length);
        debug_log("thread", "handle_request: pre_len=%lu",
                  (unsigned long)pre_len);

        if (vfs_normalize_path(req->target, path, sizeof(path)) != VFS_OK) {
                debug_log("thread", "handle_request: normalize FAILED");

                return send_response(fd, 400, "Bad Request", "text/plain",
                                     "bad request\n");
        }

        len = strlen(path);

        while (len > 1 && path[len - 1] == '/') path[--len] = '\0';

        /*
         * CGI must be checked before static paths so scripts can never be
         * served as raw files.
         */
        if (strncmp(path, CGI_PREFIX, sizeof(CGI_PREFIX) - 1) == 0) {
                const struct vfs_entry* ce;

                if (cgi_match(vfs, path, &ce) == VFS_OK)
                        return handle_cgi(fd, vfs, ce, req, pre, pre_len);

                return send_response(fd, 404, "Not Found", "text/plain",
                                     "not found\n");
        }

        if (req->method != REQUEST_METHOD_GET &&
            req->method != REQUEST_METHOD_HEAD) {
                return send_405(fd);
        }

        entry = vfs_lookup(vfs, path);

        if (entry == NULL) {
                return send_response(fd, 404, "Not Found", "text/plain",
                                     "not found\n");
        }

        if (entry->type == VFS_DIRECTORY) {
                entry = find_index(vfs, path);

                if (entry == NULL) {
                        return send_response(fd, 404, "Not Found", "text/plain",
                                             "not found\n");
                }
        }

        if (entry->type != VFS_FILE) {
                return send_response(fd, 404, "Not Found", "text/plain",
                                     "not found\n");
        }

        cached = cache_lookup(cache, entry->url_path);

        if (cached != NULL) {
                return send_cached_file(fd, cached,
                                        req->method == REQUEST_METHOD_HEAD);
        }

        errno   = 0;
        file_fd = vfs_server_open(vfs_server, entry);

        if (file_fd < 0) {
                if (errno == ENOENT || errno == ENOTDIR || errno == ELOOP ||
                    errno == EACCES) {
                        return send_response(fd, 404, "Not Found", "text/plain",
                                             "not found\n");
                }

                return send_response(fd, 500, "Internal Server Error",
                                     "text/plain", "internal server error\n");
        }

        return send_file(fd, file_fd, entry->url_path,
                         req->method == REQUEST_METHOD_HEAD);
}

static s4 handle_client(int fd, struct vfs* vfs, struct vfs_server* vfs_server,
                        struct cache* cache) {
        char buf[SERVER_REQUEST_BUF];
        struct http_request* req;
        size_t used;
        size_t consumed;
        ssize_t n;
        s4 rc;
        time_t deadline;

        debug_log("thread", "handle_client: ENTER");
        debug_log("thread", "handle_client: fd=%d", fd);

        req = (struct http_request*)malloc(sizeof(*req));

        if (req == NULL) {
                debug_log("thread", "handle_client: malloc failed: %s",
                          strerror(errno));
                return -1;
        }

        used     = 0;
        consumed = 0;
        deadline = time(NULL) + SERVER_REQUEST_TIMEOUT_SECS;

        for (;;) {
                if (time(NULL) >= deadline) {
                        debug_log("thread",
                                  "handle_client: REQUEST DEADLINE EXCEEDED");
                        free(req);
                        return -1;
                }

                debug_log("thread",
                          "handle_client: recv buffer used=%lu capacity=%lu",
                          (unsigned long)used, (unsigned long)sizeof(buf));

                n = client_recv(fd, buf + used, sizeof(buf) - used, 0);

                debug_log("thread",
                          "handle_client: AFTER recv n=%ld errno=%d (%s)",
                          (long)n, errno, strerror(errno));

                if (n < 0) {
                        if (errno == EINTR) continue;

                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                debug_log("thread",
                                          "handle_client: recv TIMEOUT");
                        } else {
                                debug_log("thread",
                                          "handle_client: recv FAILED");
                        }

                        free(req);
                        return -1;
                }

                if (n == 0) {
                        debug_log("thread", "handle_client: peer CLOSED");
                        free(req);
                        return 0;
                }

                used += (size_t)n;

                rc = request_parse(buf, used, req, &consumed);

                debug_log("thread",
                          "handle_client: AFTER request_parse rc=%d "
                          "consumed=%lu",
                          (int)rc, (unsigned long)consumed);

                if (rc == REQUEST_PARSE_INCOMPLETE) {
                        if (used == sizeof(buf)) {
                                send_response(
                                    fd, 431, "Request Header Fields Too Large",
                                    "text/plain", "request too large\n");

                                free(req);
                                return -1;
                        }

                        continue;
                }

                if (rc == REQUEST_PARSE_TOO_LARGE) {
                        send_response(fd, 431,
                                      "Request Header Fields Too Large",
                                      "text/plain", "request too large\n");

                        free(req);
                        return -1;
                }

                if (rc != REQUEST_PARSE_OK) {
                        send_response(fd, 400, "Bad Request", "text/plain",
                                      "bad request\n");

                        free(req);
                        return -1;
                }

                debug_log("thread", "handle_client: request parsed");
                debug_log("thread", "handle_client: target='%s'", req->target);
                debug_log("thread", "handle_client: method=%s",
                          method_name(req->method));
                debug_log("thread", "handle_client: content_length=%lu",
                          (unsigned long)req->content_length);

                break;
        }

        rc = handle_request(fd, vfs, vfs_server, cache, req, buf + consumed,
                            used - consumed);

        debug_log("thread", "handle_client: AFTER handle_request rc=%d",
                  (int)rc);

        free(req);

        return rc;
}

static void* server_thread_main(void* arg) {
        struct server_thread_args* args;
        int client_fd;
        struct vfs* vfs;
        struct vfs_server* vfs_server;
        struct cache* cache;
        s4 handle_rc;
#ifdef ENABLE_TLS
        struct tls_context* tls_ctx;
        struct tls_connection* tls_conn;
        int key_rc;
#endif

        debug_log("thread", "server_thread_main: ENTER");

        args = (struct server_thread_args*)arg;

        if (args == NULL) {
                debug_log("thread", "server_thread_main: args is NULL");
                return NULL;
        }

        client_fd  = args->client_fd;
        vfs        = args->vfs;
        vfs_server = args->vfs_server;
        cache      = args->cache;

#ifdef ENABLE_TLS
        tls_ctx  = args->tls_ctx;
        tls_conn = NULL;
#endif

        if (set_socket_timeouts(client_fd) < 0) {
                debug_log("thread",
                          "server_thread_main: set_socket_timeouts FAILED");

                close(client_fd);

                pthread_mutex_lock(&thread_mutex);
                thread_count--;
                pthread_mutex_unlock(&thread_mutex);

                free(args);
                return NULL;
        }

#ifdef ENABLE_TLS
        key_rc =
            pthread_once(&tls_connection_key_once, create_tls_connection_key);

        if (key_rc != 0 || tls_connection_key_status != 0) {
                debug_log("thread",
                          "server_thread_main: TLS pthread key creation "
                          "failed");

                close(client_fd);

                pthread_mutex_lock(&thread_mutex);
                thread_count--;
                pthread_mutex_unlock(&thread_mutex);

                free(args);
                return NULL;
        }

        if (tls_ctx == NULL) {
                debug_log("thread", "server_thread_main: missing TLS context");

                close(client_fd);

                pthread_mutex_lock(&thread_mutex);
                thread_count--;
                pthread_mutex_unlock(&thread_mutex);

                free(args);
                return NULL;
        }

        debug_log("thread", "server_thread_main: starting TLS handshake fd=%d",
                  client_fd);

        if (tls_connection_accept(tls_ctx, client_fd, &tls_conn) != 0) {
                const char* error_text;

                error_text = tls_last_error();

                debug_log(
                    "thread", "server_thread_main: TLS handshake failed: %s",
                    error_text != NULL ? error_text : "unknown TLS error");

                if (tls_conn != NULL) {
                        tls_connection_destroy(tls_conn);
                        tls_conn = NULL;
                }

                close(client_fd);

                pthread_mutex_lock(&thread_mutex);
                thread_count--;
                pthread_mutex_unlock(&thread_mutex);

                free(args);
                return NULL;
        }

        if (tls_conn == NULL) {
                debug_log("thread",
                          "server_thread_main: TLS accept returned no "
                          "connection");

                close(client_fd);

                pthread_mutex_lock(&thread_mutex);
                thread_count--;
                pthread_mutex_unlock(&thread_mutex);

                free(args);
                return NULL;
        }

        key_rc = pthread_setspecific(tls_connection_key, tls_conn);

        if (key_rc != 0) {
                debug_log("thread",
                          "server_thread_main: pthread_setspecific failed "
                          "rc=%d",
                          key_rc);

                (void)tls_connection_shutdown(tls_conn);
                tls_connection_destroy(tls_conn);
                close(client_fd);

                pthread_mutex_lock(&thread_mutex);
                thread_count--;
                pthread_mutex_unlock(&thread_mutex);

                free(args);
                return NULL;
        }

        debug_log("thread", "server_thread_main: TLS handshake complete");
#endif

        debug_log("thread", "server_thread_main: BEFORE handle_client");

        handle_rc = handle_client(client_fd, vfs, vfs_server, cache);

        debug_log("thread", "server_thread_main: AFTER handle_client rc=%d",
                  (int)handle_rc);

#ifdef ENABLE_TLS
        /*
         * Stop exposing this connection through thread-specific storage
         * before shutting it down and freeing its TLS session.
         */
        (void)pthread_setspecific(tls_connection_key, NULL);
        (void)tls_connection_shutdown(tls_conn);
        tls_connection_destroy(tls_conn);
        tls_conn = NULL;
#endif

        debug_log("thread", "server_thread_main: closing client fd=%d",
                  client_fd);

        close(client_fd);

        pthread_mutex_lock(&thread_mutex);
        thread_count--;
        debug_log("thread", "server_thread_main: thread_count=%d",
                  (int)thread_count);
        pthread_mutex_unlock(&thread_mutex);

        free(args);

        debug_log("thread", "server_thread_main: EXIT");

        return NULL;
}

#ifdef ENABLE_TLS
s4 server_thread_start(int client_fd, struct vfs* vfs,
                       struct vfs_server* vfs_server, struct cache* cache,
                       struct tls_context* tls_ctx)
#else
s4 server_thread_start(int client_fd, struct vfs* vfs,
                       struct vfs_server* vfs_server, struct cache* cache)
#endif
{
        pthread_t thread;
        pthread_attr_t attr;
        struct server_thread_args* args;
        int rc;

        debug_log("thread", "server_thread_start: ENTER");

        pthread_mutex_lock(&thread_mutex);

        debug_log("thread", "server_thread_start: current thread_count=%d",
                  (int)thread_count);

        if (thread_count >= SERVER_MAX_THREADS) {
                pthread_mutex_unlock(&thread_mutex);

                debug_log("thread",
                          "server_thread_start: THREAD LIMIT REACHED "
                          "count=%d max=%d",
                          (int)thread_count, SERVER_MAX_THREADS);
                return -1;
        }

        thread_count++;

        pthread_mutex_unlock(&thread_mutex);

        args = (struct server_thread_args*)malloc(sizeof(*args));

        if (args == NULL) {
                debug_log("thread",
                          "server_thread_start: malloc FAILED errno=%d (%s)",
                          errno, strerror(errno));

                pthread_mutex_lock(&thread_mutex);
                thread_count--;
                pthread_mutex_unlock(&thread_mutex);

                return -1;
        }

        args->client_fd  = client_fd;
        args->vfs        = vfs;
        args->vfs_server = vfs_server;
        args->cache      = cache;

#ifdef ENABLE_TLS
        args->tls_ctx = tls_ctx;
#endif

        rc = pthread_attr_init(&attr);

        if (rc != 0) {
                debug_log("thread",
                          "server_thread_start: pthread_attr_init FAILED "
                          "rc=%d",
                          rc);

                pthread_mutex_lock(&thread_mutex);
                thread_count--;
                pthread_mutex_unlock(&thread_mutex);

                free(args);
                return -1;
        }

        rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        if (rc != 0) {
                debug_log("thread",
                          "server_thread_start: pthread_attr_setdetachstate "
                          "FAILED rc=%d",
                          rc);

                pthread_attr_destroy(&attr);

                pthread_mutex_lock(&thread_mutex);
                thread_count--;
                pthread_mutex_unlock(&thread_mutex);

                free(args);
                return -1;
        }

        rc = pthread_create(&thread, &attr, server_thread_main, args);

        pthread_attr_destroy(&attr);

        if (rc != 0) {
                debug_log("thread",
                          "server_thread_start: pthread_create FAILED rc=%d",
                          rc);

                pthread_mutex_lock(&thread_mutex);
                thread_count--;
                pthread_mutex_unlock(&thread_mutex);

                free(args);
                return -1;
        }

        debug_log("thread", "server_thread_start: SUCCESS thread created");

        return 0;
}
