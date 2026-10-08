#include "cgi-bin.h"
#include "allowed_chars.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static int write_all(int fd, const char* buf, size_t len) {
        while (len > 0) {
                ssize_t n;

                n = write(fd, buf, len);

                if (n < 0) {
                        if (errno == EINTR)
                                continue;

                        return -1;
                }

                if (n == 0)
                        return -1;

                buf += n;
                len -= (size_t)n;
        }

        return 0;
}

static void send_error(int fd, int code, const char* text) {
        char buf[256];
        int n;
        size_t text_len;

        if (text == NULL)
                text = "Error";

        text_len = strlen(text);

        n = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: %lu\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "%s\n",
                     code,
                     text,
                     (unsigned long)(text_len + 1),
                     text);

        if (n > 0 && (size_t)n < sizeof(buf))
                (void)write_all(fd, buf, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* header helpers                                                     */
/* ------------------------------------------------------------------ */

static int header_name_equal(const struct request_header* h,
                             const char* name) {
        size_t i;
        size_t len;

        if (h == NULL || name == NULL)
                return 0;

        len = strlen(name);

        if (h->name == NULL || h->name_len != len)
                return 0;

        for (i = 0; i < len; i++) {
                unsigned char a;
                unsigned char b;

                a = (unsigned char)h->name[i];
                b = (unsigned char)name[i];

                if (a >= 'A' && a <= 'Z')
                        a = (unsigned char)(a - 'A' + 'a');

                if (b >= 'A' && b <= 'Z')
                        b = (unsigned char)(b - 'A' + 'a');

                if (a != b)
                        return 0;
        }

        return 1;
}

static const struct request_header*
header_get(const struct cgi_request* req, const char* name) {
        size_t i;

        if (req == NULL || name == NULL)
                return NULL;

        for (i = 0; i < req->header_count; i++) {
                if (header_name_equal(&req->headers[i], name))
                        return &req->headers[i];
        }

        return NULL;
}

static int valid_env_value_n(const char* value, size_t len) {
        size_t i;

        if (value == NULL)
                return 0;

        for (i = 0; i < len; i++) {
                if (!ac_is_value((unsigned char)value[i]))
                        return 0;
        }

        return 1;
}

/* ------------------------------------------------------------------ */
/* environment                                                        */
/* ------------------------------------------------------------------ */

static char* make_var_n(const char* name,
                        size_t name_len,
                        const char* value,
                        size_t value_len) {
        char* s;
        size_t total;

        if (name == NULL)
                return NULL;

        if (value == NULL) {
                value = "";
                value_len = 0;
        }

        if (name_len > SIZE_MAX - value_len - 2)
                return NULL;

        total = name_len + 1 + value_len + 1;

        s = (char*)malloc(total);

        if (s == NULL)
                return NULL;

        memcpy(s, name, name_len);
        s[name_len] = '=';

        if (value_len > 0)
                memcpy(s + name_len + 1, value, value_len);

        s[name_len + 1 + value_len] = '\0';

        return s;
}

static void env_free(char** env) {
        size_t i;

        if (env == NULL)
                return;

        for (i = 0; env[i] != NULL; i++)
                free(env[i]);

        free(env);
}

static int env_add_n(char** env,
                     size_t* count,
                     size_t* bytes,
                     const char* name,
                     size_t name_len,
                     const char* value,
                     size_t value_len) {
        char* var;
        size_t len;

        if (env == NULL || count == NULL || bytes == NULL)
                return -1;

        if (*count >= CGI_MAX_ENV_VARS)
                return -1;

        var = make_var_n(name, name_len, value, value_len);

        if (var == NULL)
                return -1;

        len = strlen(var) + 1;

        if (*bytes > CGI_MAX_ENV_BYTES ||
            len > CGI_MAX_ENV_BYTES - *bytes) {
                free(var);
                return -1;
        }

        env[*count] = var;
        (*count)++;
        *bytes += len;
        env[*count] = NULL;

        return 0;
}

static int env_add(char** env,
                   size_t* count,
                   size_t* bytes,
                   const char* name,
                   const char* value) {
        if (value == NULL)
                value = "";

        return env_add_n(env,
                         count,
                         bytes,
                         name,
                         strlen(name),
                         value,
                         strlen(value));
}

/*
 * Convert:
 *
 *     Host            -> HTTP_HOST
 *     User-Agent      -> HTTP_USER_AGENT
 *     X-Test          -> HTTP_X_TEST
 */
static int make_http_name(const char* name,
                          size_t name_len,
                          char* out,
                          size_t out_size) {
        size_t i;

        if (name == NULL || out == NULL || out_size < 6)
                return -1;

        if (name_len > out_size - 6)
                return -1;

        memcpy(out, "HTTP_", 5);

        for (i = 0; i < name_len; i++) {
                unsigned char c;

                c = (unsigned char)name[i];

                if (c >= 'a' && c <= 'z') {
                        out[5 + i] =
                            (char)(c - 'a' + 'A');
                } else if (c >= 'A' && c <= 'Z') {
                        out[5 + i] = (char)c;
                } else if (c >= '0' && c <= '9') {
                        out[5 + i] = (char)c;
                } else if (c == '-') {
                        out[5 + i] = '_';
                } else {
                        return -1;
                }
        }

        out[5 + name_len] = '\0';

        return 0;
}

/* ------------------------------------------------------------------ */
/* CGI environment                                                    */
/* ------------------------------------------------------------------ */

static char** env_build(const struct vfs_entry* entry,
                        const struct cgi_request* req,
                        const char* script_filename) {
        char** env;
        char clen[32];
        char http_name[256];
        const struct request_header* h;
        size_t count;
        size_t bytes;
        size_t i;

        if (entry == NULL ||
            req == NULL ||
            script_filename == NULL)
                return NULL;

        if (req->header_count > 0 && req->headers == NULL)
                return NULL;

        env = (char**)calloc(CGI_MAX_ENV_VARS + 1, sizeof(*env));

        if (env == NULL)
                return NULL;

        count = 0;
        bytes = 0;

        if (snprintf(clen,
                     sizeof(clen),
                     "%lu",
                     (unsigned long)req->body_len) < 0)
                goto fail;

#define ADD_ENV(n, v)                                                       \
        do {                                                                \
                const char* _v = (v);                                       \
                if (_v == NULL ||                                           \
                    !valid_env_value_n(_v, strlen(_v)) ||                   \
                    env_add(env, &count, &bytes, (n), _v) != 0)             \
                        goto fail;                                          \
        } while (0)

        /*
         * Standard CGI variables.
         */
        ADD_ENV("GATEWAY_INTERFACE", "CGI/1.1");
        ADD_ENV("SERVER_SOFTWARE", "civet");
        ADD_ENV("SERVER_PROTOCOL", "HTTP/1.1");

        ADD_ENV("SERVER_NAME", req->server_name);
        ADD_ENV("SERVER_PORT", req->server_port);

        ADD_ENV("REQUEST_METHOD", req->method);
        ADD_ENV("SCRIPT_NAME", entry->url_path);
        ADD_ENV("SCRIPT_FILENAME", script_filename);
        ADD_ENV("QUERY_STRING", req->query);

        /*
         * Content-Type is special in CGI.
         */
        h = header_get(req, "Content-Type");

        if (h != NULL) {
                if (!valid_env_value_n(h->value, h->value_len))
                        goto fail;

                if (env_add_n(env,
                              &count,
                              &bytes,
                              "CONTENT_TYPE",
                              strlen("CONTENT_TYPE"),
                              h->value,
                              h->value_len) != 0)
                        goto fail;
        } else {
                ADD_ENV("CONTENT_TYPE", req->content_type);
        }

        /*
         * Always describe the body we actually pass to the CGI.
         */
        ADD_ENV("CONTENT_LENGTH", clen);

        ADD_ENV("REMOTE_ADDR", req->remote_addr);

        /*
         * Deliberately use a controlled PATH.
         */
        ADD_ENV("PATH", "/usr/bin:/bin");

        /*
         * Convert ordinary request headers to HTTP_*.
         *
         * Content-Type and Content-Length have dedicated CGI
         * variables.
         *
         * Proxy is deliberately excluded because:
         *
         *     Proxy: evil
         *
         * would otherwise become:
         *
         *     HTTP_PROXY=evil
         *
         * which is interpreted as a proxy configuration variable by
         * some subprocess/network libraries.
         */
        for (i = 0; i < req->header_count; i++) {
                h = &req->headers[i];

                if (h->name == NULL || h->value == NULL)
                        goto fail;

                if (!valid_env_value_n(h->value, h->value_len))
                        goto fail;

                if (header_name_equal(h, "Content-Type") ||
                    header_name_equal(h, "Content-Length"))
                        continue;

                if (header_name_equal(h, "Proxy"))
                        continue;

                if (make_http_name(h->name,
                                   h->name_len,
                                   http_name,
                                   sizeof(http_name)) != 0)
                        goto fail;

                if (env_add_n(env,
                              &count,
                              &bytes,
                              http_name,
                              strlen(http_name),
                              h->value,
                              h->value_len) != 0)
                        goto fail;
        }

#undef ADD_ENV

        return env;

fail:
        env_free(env);
        return NULL;
}

/* ------------------------------------------------------------------ */
/* matching                                                           */
/* ------------------------------------------------------------------ */

static s4 disk_path_of(const struct vfs* vfs,
                       const struct vfs_entry* entry,
                       char* out,
                       size_t out_size) {
        int n;

        if (vfs == NULL || entry == NULL || out == NULL)
                return VFS_ERROR;

        n = snprintf(out,
                     out_size,
                     "%s/%s",
                     vfs->root,
                     entry->rel_path);

        if (n < 0 || (size_t)n >= out_size)
                return VFS_BAD_PATH;

        return VFS_OK;
}

s4 cgi_match(const struct vfs* vfs,
             const char* url_path,
             const struct vfs_entry** out) {
        const struct vfs_entry* entry;
        char disk[PATH_MAX];

        if (vfs == NULL || url_path == NULL || out == NULL)
                return VFS_ERROR;

        *out = NULL;

        if (strncmp(url_path,
                    CGI_PREFIX,
                    sizeof(CGI_PREFIX) - 1) != 0)
                return VFS_NOT_FOUND;

        entry = vfs_lookup(vfs, url_path);

        if (entry == NULL || entry->type != VFS_FILE)
                return VFS_NOT_FOUND;

        if (disk_path_of(vfs,
                         entry,
                         disk,
                         sizeof(disk)) != VFS_OK)
                return VFS_NOT_FOUND;

        if (access(disk, X_OK) != 0)
                return VFS_NOT_FOUND;

        *out = entry;

        return VFS_OK;
}

/* ------------------------------------------------------------------ */
/* CGI output parsing                                                 */
/* ------------------------------------------------------------------ */

static int find_header_end(const char* buf,
                           size_t len,
                           size_t* hdr_len,
                           size_t* body_off) {
        size_t i;

        if (buf == NULL || hdr_len == NULL || body_off == NULL)
                return 0;

        /*
         * Accept the CGI conventions:
         *
         *     \r\n\r\n
         *     \n\n
         */
        for (i = 0; i + 1 < len; i++) {
                if (buf[i] == '\n' &&
                    buf[i + 1] == '\n') {
                        *hdr_len = i;
                        *body_off = i + 2;
                        return 1;
                }

                if (i + 3 < len &&
                    buf[i] == '\r' &&
                    buf[i + 1] == '\n' &&
                    buf[i + 2] == '\r' &&
                    buf[i + 3] == '\n') {
                        *hdr_len = i;
                        *body_off = i + 4;
                        return 1;
                }
        }

        return 0;
}

static int valid_status(const char* status) {
        size_t i;
        size_t n;

        if (status == NULL)
                return 0;

        n = strlen(status);

        /*
         * CGI Status is:
         *
         *     3DIGIT SP reason
         */
        if (n < 3)
                return 0;

        if (!ac_is_digit((unsigned char)status[0]) ||
            !ac_is_digit((unsigned char)status[1]) ||
            !ac_is_digit((unsigned char)status[2]))
                return 0;

        if (status[0] < '1' || status[0] > '5')
                return 0;

        if (n == 3)
                return 1;

        if (status[3] != ' ')
                return 0;

        for (i = 4; i < n; i++) {
                if (!ac_is_value((unsigned char)status[i]))
                        return 0;
        }

        return 1;
}

static int valid_response_header(const char* line, size_t len) {
        size_t i;
        size_t colon;

        if (line == NULL || len == 0)
                return 0;

        colon = 0;

        while (colon < len && line[colon] != ':')
                colon++;

        if (colon == 0 || colon == len)
                return 0;

        /*
         * field-name = token
         */
        for (i = 0; i < colon; i++) {
                if (!ac_is_token((unsigned char)line[i]))
                        return 0;
        }

        /*
         * field-value = allowed HTTP value characters.
         */
        for (i = colon + 1; i < len; i++) {
                if (!ac_is_value((unsigned char)line[i]))
                        return 0;
        }

        return 1;
}

static int header_is(const char* line,
                     size_t len,
                     const char* name) {
        size_t name_len;

        if (line == NULL || name == NULL)
                return 0;

        name_len = strlen(name);

        if (len <= name_len)
                return 0;

        if (line[name_len] != ':')
                return 0;

        return strncasecmp(line, name, name_len) == 0;
}

static int is_hop_by_hop_header(const char* line, size_t len) {
        if (header_is(line, len, "Connection") ||
            header_is(line, len, "Proxy-Connection") ||
            header_is(line, len, "Transfer-Encoding") ||
            header_is(line, len, "Keep-Alive") ||
            header_is(line, len, "Upgrade") ||
            header_is(line, len, "TE") ||
            header_is(line, len, "Trailer") ||
            header_is(line, len, "Content-Length"))
                return 1;

        return 0;
}

static s4 send_response(int fd,
                        const char* buf,
                        size_t len,
                        int head_only) {
        size_t hdr_len;
        size_t body_off;
        size_t pos;
        size_t eol;
        size_t line_len;
        size_t body_len;
        size_t status_len;
        char status[64];
        char head[256];
        int have_location;
        char* hdrs;
        size_t hdrs_len;

        if (fd < 0 || buf == NULL)
                return VFS_ERROR;

        if (!find_header_end(buf,
                             len,
                             &hdr_len,
                             &body_off))
                return VFS_ERROR;

        /*
         * Worst case is approximately the complete CGI header block
         * plus CRLF per line.
         */
        if (hdr_len > SIZE_MAX - 2)
                return VFS_ERROR;

        hdrs = (char*)malloc(hdr_len + 2);

        if (hdrs == NULL)
                return VFS_ERROR;

        hdrs_len = 0;
        have_location = 0;

        strcpy(status, "200 OK");
        status_len = strlen(status);

        pos = 0;

        while (pos < hdr_len) {
                eol = pos;

                while (eol < hdr_len && buf[eol] != '\n')
                        eol++;

                if (eol == hdr_len) {
                        free(hdrs);
                        return VFS_ERROR;
                }

                line_len = eol - pos;

                if (line_len > 0 &&
                    buf[pos + line_len - 1] == '\r')
                        line_len--;

                if (line_len == 0) {
                        free(hdrs);
                        return VFS_ERROR;
                }

                /*
                 * CGI Status header.
                 */
                if (header_is(buf + pos, line_len, "Status")) {
                        size_t s;
                        size_t n;

                        s = pos + strlen("Status") + 1;

                        while (s < pos + line_len &&
                               (buf[s] == ' ' ||
                                buf[s] == '\t'))
                                s++;

                        n = pos + line_len - s;

                        if (n == 0 || n >= sizeof(status)) {
                                free(hdrs);
                                return VFS_ERROR;
                        }

                        memcpy(status, buf + s, n);
                        status[n] = '\0';

                        if (!valid_status(status)) {
                                free(hdrs);
                                return VFS_ERROR;
                        }

                        status_len = n;
                } else {
                        /*
                         * Validate before doing anything with the line.
                         */
                        if (!valid_response_header(buf + pos,
                                                   line_len)) {
                                free(hdrs);
                                return VFS_ERROR;
                        }

                        if (header_is(buf + pos,
                                      line_len,
                                      "Location")) {
                                have_location = 1;
                        }

                        /*
                         * We always provide our own message framing.
                         * Never allow CGI to control it.
                         */
                        if (!is_hop_by_hop_header(buf + pos,
                                                  line_len)) {
                                if (hdrs_len >
                                    hdr_len - line_len - 2) {
                                        free(hdrs);
                                        return VFS_ERROR;
                                }

                                memcpy(hdrs + hdrs_len,
                                       buf + pos,
                                       line_len);

                                hdrs_len += line_len;
                                hdrs[hdrs_len++] = '\r';
                                hdrs[hdrs_len++] = '\n';
                        }
                }

                pos = eol + 1;
        }

        /*
         * CGI specifies that Location without an explicit Status
         * results in a redirect.
         */
        if (have_location &&
            strcmp(status, "200 OK") == 0) {
                strcpy(status, "302 Found");
                status_len = strlen(status);
        }

        body_len = len - body_off;

        if ((size_t)snprintf(head,
                             sizeof(head),
                             "HTTP/1.1 %.*s\r\n",
                             (int)status_len,
                             status) >= sizeof(head)) {
                free(hdrs);
                return VFS_ERROR;
        }

        if (write_all(fd, head, strlen(head)) != 0)
                goto fail;

        if (write_all(fd, hdrs, hdrs_len) != 0)
                goto fail;

        if ((size_t)snprintf(head,
                             sizeof(head),
                             "Content-Length: %lu\r\n"
                             "Connection: close\r\n"
                             "\r\n",
                             (unsigned long)body_len) >= sizeof(head))
                goto fail;

        if (write_all(fd, head, strlen(head)) != 0)
                goto fail;

        if (!head_only &&
            write_all(fd, buf + body_off, body_len) != 0)
                goto fail;

        free(hdrs);

        return VFS_OK;

fail:
        free(hdrs);
        return VFS_ERROR;
}

/* ------------------------------------------------------------------ */
/* execution helpers                                                  */
/* ------------------------------------------------------------------ */

static int make_deadline(struct timespec* deadline,
                         int seconds) {
        if (deadline == NULL)
                return -1;

        if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0)
                return -1;

        deadline->tv_sec += seconds;

        return 0;
}

static int deadline_expired(const struct timespec* deadline) {
        struct timespec now;

        if (deadline == NULL)
                return 1;

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
                return 1;

        if (now.tv_sec > deadline->tv_sec)
                return 1;

        if (now.tv_sec == deadline->tv_sec &&
            now.tv_nsec >= deadline->tv_nsec)
                return 1;

        return 0;
}

static int deadline_poll_ms(const struct timespec* deadline) {
        struct timespec now;
        time_t sec;
        long nsec;
        long long ms;

        if (deadline == NULL)
                return 0;

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
                return 0;

        sec = deadline->tv_sec - now.tv_sec;
        nsec = deadline->tv_nsec - now.tv_nsec;

        if (nsec < 0) {
                sec--;
                nsec += 1000000000L;
        }

        if (sec < 0)
                return 0;

        ms = (long long)sec * 1000LL +
             (long long)(nsec + 999999L) / 1000000LL;

        /*
         * Don't sleep longer than one second. This also means that
         * unexpected clock/FD state is noticed promptly.
         */
        if (ms > 1000)
                ms = 1000;

        if (ms < 0)
                ms = 0;

        return (int)ms;
}
static void kill_and_reap(pid_t pid) {
        int status;

        if (pid <= 0)
                return;

        /*
         * CGI is put into its own process group. Kill both the group
         * and the direct child; the second call is harmless if the
         * child was already killed.
         */
        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);

        while (waitpid(pid, &status, 0) < 0 &&
               errno == EINTR) {
        }
}

/* ------------------------------------------------------------------ */
/* execution                                                          */
/* ------------------------------------------------------------------ */

s4 cgi_run(const struct vfs* vfs,
           const struct vfs_entry* entry,
           const struct cgi_request* req,
           int out_fd) {
        char disk[PATH_MAX];
        char dir[PATH_MAX];
        char* argv[2];
        char** env;
        char* slash;
        char* buf;
        char* tmp;
        size_t len;
        size_t cap;
        size_t sent;
        ssize_t r;
        ssize_t w;
        long maxfd;
        int in_pipe[2];
        int out_pipe[2];
        int status;
        int eof;
        int timed_out;
        int output_limit;
        int input_error;
        int in_fd;
        int fd;
        int nf;
        int head_only;
        int flags;
        int poll_rc;
        struct pollfd pfd[2];
        struct timespec deadline;
        sigset_t pipe_set;
        sigset_t old_set;
        sigset_t pending;
        struct timespec zero;
        pid_t pid;

        if (vfs == NULL ||
            entry == NULL ||
            req == NULL ||
            out_fd < 0)
                return VFS_ERROR;

        if (req->body_len > CGI_MAX_BODY) {
                send_error(out_fd,
                           413,
                           "Request Entity Too Large");
                return VFS_ERROR;
        }

        if (req->body_len > 0 &&
            req->body == NULL) {
                send_error(out_fd,
                           400,
                           "Bad Request");
                return VFS_ERROR;
        }

        head_only = req->method != NULL &&
                    strcmp(req->method, "HEAD") == 0;

        if (disk_path_of(vfs,
                         entry,
                         disk,
                         sizeof(disk)) != VFS_OK) {
                send_error(out_fd,
                           500,
                           "Internal Server Error");
                return VFS_BAD_PATH;
        }

        if (strlen(disk) >= sizeof(dir)) {
                send_error(out_fd,
                           500,
                           "Internal Server Error");
                return VFS_BAD_PATH;
        }

        strcpy(dir, disk);

        slash = strrchr(dir, '/');

        if (slash == NULL) {
                send_error(out_fd,
                           500,
                           "Internal Server Error");
                return VFS_BAD_PATH;
        }

        *slash = '\0';

        env = env_build(entry, req, disk);

        if (env == NULL) {
                send_error(out_fd,
                           500,
                           "Internal Server Error");
                return VFS_ERROR;
        }

        cap = 4096;

        if (cap > CGI_MAX_OUTPUT)
                cap = CGI_MAX_OUTPUT;

        buf = (char*)malloc(cap);

        if (buf == NULL) {
                env_free(env);
                send_error(out_fd,
                           500,
                           "Internal Server Error");
                return VFS_ERROR;
        }

        if (pipe(in_pipe) != 0) {
                free(buf);
                env_free(env);
                send_error(out_fd,
                           500,
                           "Internal Server Error");
                return VFS_ERROR;
        }

        if (pipe(out_pipe) != 0) {
                close(in_pipe[0]);
                close(in_pipe[1]);

                free(buf);
                env_free(env);

                send_error(out_fd,
                           500,
                           "Internal Server Error");
                return VFS_ERROR;
        }

        /*
         * The CGI child expects these descriptors to be ordinary
         * descriptors which can safely be dup2()'d onto stdin/stdout.
         *
         * If pipe() returned 0/1/2 because the server had one of those
         * descriptors closed, normalize that situation first.
         */
        if (in_pipe[0] <= STDERR_FILENO ||
            in_pipe[1] <= STDERR_FILENO ||
            out_pipe[0] <= STDERR_FILENO ||
            out_pipe[1] <= STDERR_FILENO) {
                close(in_pipe[0]);
                close(in_pipe[1]);
                close(out_pipe[0]);
                close(out_pipe[1]);

                free(buf);
                env_free(env);

                send_error(out_fd,
                           500,
                           "Internal Server Error");
                return VFS_ERROR;
        }

        maxfd = sysconf(_SC_OPEN_MAX);

        if (maxfd < 0 || maxfd > 4096)
                maxfd = 4096;

        argv[0] = disk;
        argv[1] = NULL;

        pid = fork();

        if (pid < 0) {
                close(in_pipe[0]);
                close(in_pipe[1]);
                close(out_pipe[0]);
                close(out_pipe[1]);

                free(buf);
                env_free(env);

                send_error(out_fd,
                           500,
                           "Internal Server Error");
                return VFS_ERROR;
        }

        if (pid == 0) {
                /*
                 * Put CGI and its descendants into their own process
                 * group. The parent can then kill the whole group on
                 * timeout.
                 */
                (void)setpgid(0, 0);

                if (dup2(in_pipe[0], STDIN_FILENO) < 0)
                        _exit(126);

                if (dup2(out_pipe[1], STDOUT_FILENO) < 0)
                        _exit(126);

                close(in_pipe[0]);
                close(in_pipe[1]);
                close(out_pipe[0]);
                close(out_pipe[1]);

                /*
                 * Do not leak server descriptors into CGI.
                 */
                for (fd = 3; fd < maxfd; fd++)
                        close(fd);

                signal(SIGPIPE, SIG_DFL);

                if (chdir(dir) != 0)
                        _exit(126);

                /*
                 * Keep this as an additional child-side timeout.
                 * The parent has the authoritative timeout as well.
                 */
                alarm(CGI_TIMEOUT);

                execve(disk, argv, env);

                _exit(127);
        }

        /*
         * Best effort from the parent side as well. This closes the
         * race where the child hasn't called setpgid() yet.
         */
        (void)setpgid(pid, pid);

        close(in_pipe[0]);
        close(out_pipe[1]);

        env_free(env);

        /*
         * Protect the server from SIGPIPE if CGI closes stdin.
         */
        sigemptyset(&pipe_set);
        sigaddset(&pipe_set, SIGPIPE);

        if (pthread_sigmask(SIG_BLOCK,
                            &pipe_set,
                            &old_set) != 0) {
                close(in_pipe[1]);
                close(out_pipe[0]);

                kill_and_reap(pid);

                free(buf);

                send_error(out_fd,
                           500,
                           "Internal Server Error");
                return VFS_ERROR;
        }

        in_fd = in_pipe[1];
        sent = 0;

        if (req->body_len == 0) {
                close(in_fd);
                in_fd = -1;
        } else {
                flags = fcntl(in_fd, F_GETFL, 0);

                if (flags < 0 ||
                    fcntl(in_fd,
                          F_SETFL,
                          flags | O_NONBLOCK) < 0) {
                        close(in_fd);
                        close(out_pipe[0]);

                        kill_and_reap(pid);

                        pthread_sigmask(SIG_SETMASK,
                                        &old_set,
                                        NULL);

                        free(buf);

                        send_error(out_fd,
                                   500,
                                   "Internal Server Error");
                        return VFS_ERROR;
                }
        }

        /*
         * CGI stdout is non-blocking so that a full pipe cannot cause
         * the server worker to block inside read().
         */
        flags = fcntl(out_pipe[0], F_GETFL, 0);

        if (flags < 0 ||
            fcntl(out_pipe[0],
                  F_SETFL,
                  flags | O_NONBLOCK) < 0) {
                if (in_fd >= 0)
                        close(in_fd);

                close(out_pipe[0]);

                kill_and_reap(pid);

                pthread_sigmask(SIG_SETMASK,
                                &old_set,
                                NULL);

                free(buf);

                send_error(out_fd,
                           500,
                           "Internal Server Error");
                return VFS_ERROR;
        }

        len = 0;
        eof = 0;
        timed_out = 0;
        output_limit = 0;
        input_error = 0;

        if (make_deadline(&deadline,
                          CGI_TIMEOUT) != 0) {
                if (in_fd >= 0)
                        close(in_fd);

                close(out_pipe[0]);

                kill_and_reap(pid);

                pthread_sigmask(SIG_SETMASK,
                                &old_set,
                                NULL);

                free(buf);

                send_error(out_fd,
                           500,
                           "Internal Server Error");
                return VFS_ERROR;
        }

        for (;;) {
                nf = 0;

                pfd[nf].fd = out_pipe[0];
                pfd[nf].events = POLLIN;
                pfd[nf].revents = 0;
                nf++;

                if (in_fd >= 0) {
                        pfd[nf].fd = in_fd;
                        pfd[nf].events = POLLOUT;
                        pfd[nf].revents = 0;
                        nf++;
                }

                if (deadline_expired(&deadline)) {
                        timed_out = 1;
                        break;
                }

                poll_rc = poll(pfd,
                               (nfds_t)nf,
                               deadline_poll_ms(&deadline));

                if (poll_rc < 0) {
                        if (errno == EINTR)
                                continue;

                        break;
                }

                if (poll_rc == 0) {
                        timed_out = 1;
                        break;
                }

                /*
                 * Feed CGI stdin.
                 */
                if (nf == 2) {
                        if (pfd[1].revents &
                            (POLLERR |
                             POLLHUP |
                             POLLNVAL)) {
                                close(in_fd);
                                in_fd = -1;
                        } else if (pfd[1].revents & POLLOUT) {
                                w = write(in_fd,
                                          req->body + sent,
                                          req->body_len - sent);

                                if (w > 0) {
                                        sent += (size_t)w;
                                } else if (w < 0 &&
                                           errno != EAGAIN &&
                                           errno != EINTR) {
                                        input_error = 1;

                                        close(in_fd);
                                        in_fd = -1;
                                }

                                if (in_fd >= 0 &&
                                    sent >= req->body_len) {
                                        close(in_fd);
                                        in_fd = -1;
                                }
                        }
                }

                /*
                 * Drain as much CGI stdout as possible.
                 */
                if (pfd[0].revents &
                    (POLLIN |
                     POLLHUP |
                     POLLERR)) {
                        for (;;) {
                                if (len == CGI_MAX_OUTPUT) {
                                        output_limit = 1;
                                        break;
                                }

                                if (len == cap) {
                                        size_t new_cap;

                                        if (cap >= CGI_MAX_OUTPUT) {
                                                new_cap = CGI_MAX_OUTPUT;
                                        } else if (cap >
                                                   CGI_MAX_OUTPUT / 2) {
                                                new_cap = CGI_MAX_OUTPUT;
                                        } else {
                                                new_cap = cap * 2;
                                        }

                                        tmp = (char*)realloc(buf,
                                                             new_cap);

                                        if (tmp == NULL)
                                                break;

                                        buf = tmp;
                                        cap = new_cap;
                                }

                                r = read(out_pipe[0],
                                         buf + len,
                                         cap - len);

                                if (r > 0) {
                                        len += (size_t)r;
                                        continue;
                                }

                                if (r == 0) {
                                        eof = 1;
                                        break;
                                }

                                if (errno == EINTR)
                                        continue;

                                if (errno == EAGAIN)
                                        break;

                                break;
                        }

                        if (output_limit || eof)
                                break;
                }
        }

        if (in_fd >= 0)
                close(in_fd);

        close(out_pipe[0]);

        /*
         * If we did not see EOF, the CGI is either still running or
         * otherwise failed to finish. Kill it.
         */
        if (!eof) {
                kill_and_reap(pid);
        } else {
                while (waitpid(pid, &status, 0) < 0 &&
                       errno == EINTR) {
                }
        }

        /*
         * A blocked SIGPIPE generated while writing CGI stdin can
         * remain pending. Consume one pending instance before restoring
         * the caller's signal mask.
         */
        if (sigpending(&pending) == 0 &&
            sigismember(&pending, SIGPIPE)) {
                zero.tv_sec = 0;
                zero.tv_nsec = 0;

                (void)sigtimedwait(&pipe_set,
                                   NULL,
                                   &zero);
        }

        pthread_sigmask(SIG_SETMASK,
                        &old_set,
                        NULL);

        if (timed_out) {
                free(buf);

                send_error(out_fd,
                           504,
                           "Gateway Timeout");

                return VFS_ERROR;
        }

        if (output_limit) {
                free(buf);

                send_error(out_fd,
                           502,
                           "CGI Output Too Large");

                return VFS_ERROR;
        }

        if (!eof) {
                free(buf);

                send_error(out_fd,
                           502,
                           "Bad Gateway");

                return VFS_ERROR;
        }

        if (input_error && len == 0) {
                free(buf);

                send_error(out_fd,
                           502,
                           "Bad Gateway");

                return VFS_ERROR;
        }

        if (len == 0) {
                free(buf);

                if (WIFSIGNALED(status) &&
                    WTERMSIG(status) == SIGALRM) {
                        send_error(out_fd,
                                   504,
                                   "Gateway Timeout");
                } else {
                        send_error(out_fd,
                                   502,
                                   "Bad Gateway");
                }

                return VFS_ERROR;
        }

        if (send_response(out_fd,
                          buf,
                          len,
                          head_only) != VFS_OK) {
                free(buf);

                send_error(out_fd,
                           502,
                           "Bad Gateway");

                return VFS_ERROR;
        }

        free(buf);

        return VFS_OK;
}
