#include "cgi-bin.h"

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

#include "allowed_chars.h"
#include "debug.h"

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static int write_all(int fd, const char* buf, size_t len) {
        size_t original_len;

        original_len = len;

        debug_log("write_all", "fd=%d len=%lu", fd, (unsigned long)len);

        while (len > 0) {
                ssize_t n;

                n = write(fd, buf, len);

                if (n < 0) {
                        if (errno == EINTR) {
                                debug_log("write_all",
                                          "fd=%d write interrupted, retrying",
                                          fd);
                                continue;
                        }

                        debug_log("write_all",
                                  "fd=%d write failed errno=%d (%s)", fd, errno,
                                  strerror(errno));

                        return -1;
                }

                if (n == 0) {
                        debug_log(
                            "write_all",
                            "fd=%d write returned zero after %lu/%lu bytes", fd,
                            (unsigned long)(original_len - len),
                            (unsigned long)original_len);
                        return -1;
                }

                debug_log("write_all", "fd=%d wrote=%ld remaining=%lu", fd,
                          (long)n, (unsigned long)(len - (size_t)n));

                buf += n;
                len -= (size_t)n;
        }

        debug_log("write_all", "fd=%d completed len=%lu", fd,
                  (unsigned long)original_len);

        return 0;
}

static void send_error(int fd, int code, const char* text) {
        char buf[256];
        int n;
        size_t text_len;

        if (text == NULL) text = "Error";

        debug_log("send_error", "fd=%d code=%d text=\"%s\"", fd, code, text);

        text_len = strlen(text);

        n = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: %lu\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "%s\n",
                     code, text, (unsigned long)(text_len + 1), text);

        if (n > 0 && (size_t)n < sizeof(buf)) {
                if (write_all(fd, buf, (size_t)n) != 0)
                        debug_log("send_error",
                                  "failed writing error response fd=%d", fd);
        } else {
                debug_log("send_error",
                          "snprintf failed/truncated code=%d n=%d", code, n);
        }
}

/* ------------------------------------------------------------------ */
/* header helpers                                                     */
/* ------------------------------------------------------------------ */

static int header_name_equal(const struct request_header* h, const char* name) {
        size_t i;
        size_t len;

        if (h == NULL || name == NULL) {
                debug_log("header_name_equal", "invalid arguments h=%p name=%p",
                          (void*)h, (void*)name);
                return 0;
        }

        len = strlen(name);

        if (h->name == NULL || h->name_len != len) return 0;

        for (i = 0; i < len; i++) {
                unsigned char a;
                unsigned char b;

                a = (unsigned char)h->name[i];
                b = (unsigned char)name[i];

                if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');

                if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');

                if (a != b) return 0;
        }

        return 1;
}

static const struct request_header* header_get(const struct cgi_request* req,
                                               const char* name) {
        size_t i;

        if (req == NULL || name == NULL) {
                debug_log("header_get", "invalid arguments req=%p name=%p",
                          (void*)req, (void*)name);
                return NULL;
        }

        debug_log("header_get", "looking for header \"%s\" among %lu headers",
                  name, (unsigned long)req->header_count);

        for (i = 0; i < req->header_count; i++) {
                if (header_name_equal(&req->headers[i], name)) {
                        debug_log("header_get",
                                  "found header \"%s\" at index=%lu", name,
                                  (unsigned long)i);
                        return &req->headers[i];
                }
        }

        debug_log("header_get", "header \"%s\" not found", name);

        return NULL;
}

static int valid_env_value_n(const char* value, size_t len) {
        size_t i;

        if (value == NULL) {
                debug_log("valid_env_value_n", "NULL value");
                return 0;
        }

        for (i = 0; i < len; i++) {
                if (!ac_is_value((unsigned char)value[i])) {
                        debug_log("valid_env_value_n",
                                  "invalid environment byte at offset=%lu "
                                  "value=0x%02x",
                                  (unsigned long)i,
                                  (unsigned int)(unsigned char)value[i]);
                        return 0;
                }
        }

        return 1;
}

/* ------------------------------------------------------------------ */
/* environment                                                        */
/* ------------------------------------------------------------------ */

static char* make_var_n(const char* name, size_t name_len, const char* value,
                        size_t value_len) {
        char* s;
        size_t total;

        debug_log("make_var_n", "name_len=%lu value_len=%lu",
                  (unsigned long)name_len, (unsigned long)value_len);

        if (name == NULL) {
                debug_log("make_var_n", "name is NULL");
                return NULL;
        }

        if (value == NULL) {
                value     = "";
                value_len = 0;
        }

        if (name_len > SIZE_MAX - value_len - 2) {
                debug_log("make_var_n",
                          "size overflow name_len=%lu value_len=%lu",
                          (unsigned long)name_len, (unsigned long)value_len);
                return NULL;
        }

        total = name_len + 1 + value_len + 1;

        s = (char*)malloc(total);

        if (s == NULL) {
                debug_log("make_var_n", "malloc(%lu) failed",
                          (unsigned long)total);
                return NULL;
        }

        memcpy(s, name, name_len);
        s[name_len] = '=';

        if (value_len > 0) memcpy(s + name_len + 1, value, value_len);

        s[name_len + 1 + value_len] = '\0';

        return s;
}

static void env_free(char** env) {
        size_t i;

        if (env == NULL) return;

        debug_log("env_free", "freeing environment");

        for (i = 0; env[i] != NULL; i++) free(env[i]);

        free(env);

        debug_log("env_free", "environment freed entries=%lu",
                  (unsigned long)i);
}

static int env_add_n(char** env, size_t* count, size_t* bytes, const char* name,
                     size_t name_len, const char* value, size_t value_len) {
        char* var;
        size_t len;

        if (env == NULL || count == NULL || bytes == NULL) {
                debug_log("env_add_n",
                          "invalid arguments env=%p count=%p bytes=%p",
                          (void*)env, (void*)count, (void*)bytes);
                return -1;
        }

        debug_log("env_add_n",
                  "adding name_len=%lu value_len=%lu count=%lu bytes=%lu",
                  (unsigned long)name_len, (unsigned long)value_len,
                  (unsigned long)*count, (unsigned long)*bytes);

        if (*count >= CGI_MAX_ENV_VARS) {
                debug_log("env_add_n",
                          "environment variable limit reached: %lu",
                          (unsigned long)CGI_MAX_ENV_VARS);
                return -1;
        }

        var = make_var_n(name, name_len, value, value_len);

        if (var == NULL) {
                debug_log("env_add_n", "make_var_n failed");
                return -1;
        }

        len = strlen(var) + 1;

        if (*bytes > CGI_MAX_ENV_BYTES || len > CGI_MAX_ENV_BYTES - *bytes) {
                debug_log("env_add_n",
                          "environment byte limit exceeded current=%lu add=%lu "
                          "max=%lu",
                          (unsigned long)*bytes, (unsigned long)len,
                          (unsigned long)CGI_MAX_ENV_BYTES);
                free(var);
                return -1;
        }

        env[*count] = var;
        (*count)++;
        *bytes += len;
        env[*count] = NULL;

        debug_log("env_add_n", "environment entry added count=%lu bytes=%lu",
                  (unsigned long)*count, (unsigned long)*bytes);

        return 0;
}

static int env_add(char** env, size_t* count, size_t* bytes, const char* name,
                   const char* value) {
        if (value == NULL) value = "";

        return env_add_n(env, count, bytes, name, strlen(name), value,
                         strlen(value));
}

static int make_http_name(const char* name, size_t name_len, char* out,
                          size_t out_size) {
        size_t i;

        debug_log("make_http_name", "name_len=%lu out_size=%lu",
                  (unsigned long)name_len, (unsigned long)out_size);

        if (name == NULL || out == NULL || out_size < 6) {
                debug_log("make_http_name", "invalid arguments");
                return -1;
        }

        if (name_len > out_size - 6) {
                debug_log("make_http_name", "header name too long name_len=%lu",
                          (unsigned long)name_len);
                return -1;
        }

        memcpy(out, "HTTP_", 5);

        for (i = 0; i < name_len; i++) {
                unsigned char c;

                c = (unsigned char)name[i];

                if (c >= 'a' && c <= 'z') {
                        out[5 + i] = (char)(c - 'a' + 'A');
                } else if (c >= 'A' && c <= 'Z') {
                        out[5 + i] = (char)c;
                } else if (c >= '0' && c <= '9') {
                        out[5 + i] = (char)c;
                } else if (c == '-') {
                        out[5 + i] = '_';
                } else {
                        debug_log("make_http_name",
                                  "invalid header-name character offset=%lu "
                                  "byte=0x%02x",
                                  (unsigned long)i, (unsigned int)c);
                        return -1;
                }
        }

        out[5 + name_len] = '\0';

        debug_log("make_http_name", "converted header name to %s", out);

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

        debug_log("env_build", "entry=%p req=%p script_filename=%s",
                  (void*)entry, (void*)req,
                  script_filename != NULL ? script_filename : "(null)");

        if (entry == NULL || req == NULL || script_filename == NULL) {
                debug_log("env_build", "invalid required argument");
                return NULL;
        }

        if (req->header_count > 0 && req->headers == NULL) {
                debug_log("env_build", "header_count=%lu but headers=NULL",
                          (unsigned long)req->header_count);
                return NULL;
        }

        env = (char**)calloc(CGI_MAX_ENV_VARS + 1, sizeof(*env));

        if (env == NULL) {
                debug_log("env_build", "calloc environment failed");
                return NULL;
        }

        count = 0;
        bytes = 0;

        if (snprintf(clen, sizeof(clen), "%lu", (unsigned long)req->body_len) <
            0) {
                debug_log("env_build", "failed formatting content length");
                goto fail;
        }

#define ADD_ENV(n, v)                                                        \
        do {                                                                 \
                const char* _v = (v);                                        \
                debug_log("env_build", "ADD_ENV name=%s value_len=%lu", (n), \
                          (unsigned long)(_v != NULL ? strlen(_v) : 0));     \
                if (_v == NULL || !valid_env_value_n(_v, strlen(_v)) ||      \
                    env_add(env, &count, &bytes, (n), _v) != 0)              \
                        goto fail;                                           \
        } while (0)

        ADD_ENV("GATEWAY_INTERFACE", "CGI/1.1");
        ADD_ENV("SERVER_SOFTWARE", "civet");
        ADD_ENV("SERVER_PROTOCOL", "HTTP/1.1");

        ADD_ENV("SERVER_NAME", req->server_name);
        ADD_ENV("SERVER_PORT", req->server_port);

        ADD_ENV("REQUEST_METHOD", req->method);
        ADD_ENV("SCRIPT_NAME", entry->url_path);
        ADD_ENV("SCRIPT_FILENAME", script_filename);
        ADD_ENV("QUERY_STRING", req->query);

        h = header_get(req, "Content-Type");

        if (h != NULL) {
                debug_log("env_build",
                          "using request Content-Type header len=%lu",
                          (unsigned long)h->value_len);

                if (!valid_env_value_n(h->value, h->value_len)) {
                        debug_log("env_build",
                                  "Content-Type contains invalid environment "
                                  "characters");
                        goto fail;
                }

                if (env_add_n(env, &count, &bytes, "CONTENT_TYPE",
                              strlen("CONTENT_TYPE"), h->value,
                              h->value_len) != 0)
                        goto fail;
        } else {
                debug_log("env_build",
                          "no Content-Type header; using request content_type");
                ADD_ENV("CONTENT_TYPE", req->content_type);
        }

        ADD_ENV("CONTENT_LENGTH", clen);
        ADD_ENV("REMOTE_ADDR", req->remote_addr);

        ADD_ENV("PATH", "/usr/bin:/bin");

        for (i = 0; i < req->header_count; i++) {
                h = &req->headers[i];

                debug_log("env_build",
                          "processing request header index=%lu name_len=%lu "
                          "value_len=%lu",
                          (unsigned long)i, (unsigned long)h->name_len,
                          (unsigned long)h->value_len);

                if (h->name == NULL || h->value == NULL) {
                        debug_log("env_build",
                                  "header index=%lu has NULL name/value",
                                  (unsigned long)i);
                        goto fail;
                }

                if (!valid_env_value_n(h->value, h->value_len)) {
                        debug_log("env_build",
                                  "header index=%lu contains invalid value "
                                  "characters",
                                  (unsigned long)i);
                        goto fail;
                }

                if (header_name_equal(h, "Content-Type") ||
                    header_name_equal(h, "Content-Length")) {
                        debug_log("env_build",
                                  "skipping dedicated CGI header index=%lu",
                                  (unsigned long)i);
                        continue;
                }

                if (header_name_equal(h, "Proxy")) {
                        debug_log("env_build",
                                  "skipping Proxy header index=%lu",
                                  (unsigned long)i);
                        continue;
                }

                if (make_http_name(h->name, h->name_len, http_name,
                                   sizeof(http_name)) != 0) {
                        debug_log("env_build",
                                  "failed converting header index=%lu",
                                  (unsigned long)i);
                        goto fail;
                }

                if (env_add_n(env, &count, &bytes, http_name, strlen(http_name),
                              h->value, h->value_len) != 0) {
                        debug_log(
                            "env_build",
                            "failed adding header environment variable %s",
                            http_name);
                        goto fail;
                }
        }

#undef ADD_ENV

        debug_log("env_build", "success count=%lu bytes=%lu",
                  (unsigned long)count, (unsigned long)bytes);

        return env;

fail:
        debug_log("env_build", "FAILED count=%lu bytes=%lu",
                  (unsigned long)count, (unsigned long)bytes);

        env_free(env);
        return NULL;
}

/* ------------------------------------------------------------------ */
/* matching                                                           */
/* ------------------------------------------------------------------ */

static s4 disk_path_of(const struct vfs* vfs, const struct vfs_entry* entry,
                       char* out, size_t out_size) {
        int n;

        debug_log("disk_path_of", "vfs=%p entry=%p out_size=%lu", (void*)vfs,
                  (void*)entry, (unsigned long)out_size);

        if (vfs == NULL || entry == NULL || out == NULL) {
                debug_log("disk_path_of", "invalid argument");
                return VFS_ERROR;
        }

        n = snprintf(out, out_size, "%s/%s", vfs->root, entry->rel_path);

        if (n < 0 || (size_t)n >= out_size) {
                debug_log("disk_path_of", "path construction failed n=%d", n);
                return VFS_BAD_PATH;
        }

        debug_log("disk_path_of", "disk path=%s", out);

        return VFS_OK;
}

s4 cgi_match(const struct vfs* vfs, const char* url_path,
             const struct vfs_entry** out) {
        const struct vfs_entry* entry;
        char disk[PATH_MAX];

        debug_log("cgi_match", "url_path=%s",
                  url_path != NULL ? url_path : "(null)");

        if (vfs == NULL || url_path == NULL || out == NULL) {
                debug_log("cgi_match", "invalid argument");
                return VFS_ERROR;
        }

        *out = NULL;

        if (strncmp(url_path, CGI_PREFIX, sizeof(CGI_PREFIX) - 1) != 0) {
                debug_log("cgi_match", "URL does not have CGI prefix");
                return VFS_NOT_FOUND;
        }

        entry = vfs_lookup(vfs, url_path);

        if (entry == NULL) {
                debug_log("cgi_match", "vfs_lookup returned NULL");
                return VFS_NOT_FOUND;
        }

        debug_log("cgi_match", "vfs entry found type=%d", (int)entry->type);

        if (entry->type != VFS_FILE) {
                debug_log("cgi_match", "entry is not a file");
                return VFS_NOT_FOUND;
        }

        if (disk_path_of(vfs, entry, disk, sizeof(disk)) != VFS_OK) {
                debug_log("cgi_match", "disk_path_of failed");
                return VFS_NOT_FOUND;
        }

        if (access(disk, X_OK) != 0) {
                debug_log("cgi_match",
                          "CGI not executable path=%s errno=%d (%s)", disk,
                          errno, strerror(errno));
                return VFS_NOT_FOUND;
        }

        *out = entry;

        debug_log("cgi_match", "CGI MATCH path=%s", disk);

        return VFS_OK;
}

/* ------------------------------------------------------------------ */
/* CGI output parsing                                                 */
/* ------------------------------------------------------------------ */

static int find_header_end(const char* buf, size_t len, size_t* hdr_len,
                           size_t* body_off) {
        size_t i;

        debug_log("find_header_end", "len=%lu", (unsigned long)len);

        if (buf == NULL || hdr_len == NULL || body_off == NULL) {
                debug_log("find_header_end", "invalid arguments");
                return 0;
        }

        /*
         * hdr_len is the length of the complete CGI header section.
         *
         * For CRLF/CRLF:
         *
         *     Header: value\r\n\r\n
         *              ^       ^
         *              |       body_off
         *              hdr_len
         *
         * hdr_len includes the CRLF terminating the final header,
         * but excludes the empty CRLF which separates headers/body.
         *
         * For LF/LF the same rule applies: hdr_len includes the
         * final LF belonging to the last header.
         */

        for (i = 0; i + 1 < len; i++) {
                if (buf[i] == '\n' && buf[i + 1] == '\n') {
                        *hdr_len  = i + 1;
                        *body_off = i + 2;

                        debug_log("find_header_end",
                                  "found LF/LF hdr_len=%lu body_off=%lu",
                                  (unsigned long)*hdr_len,
                                  (unsigned long)*body_off);

                        return 1;
                }

                if (i + 3 < len && buf[i] == '\r' && buf[i + 1] == '\n' &&
                    buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                        *hdr_len  = i + 2;
                        *body_off = i + 4;

                        debug_log("find_header_end",
                                  "found CRLF/CRLF hdr_len=%lu body_off=%lu",
                                  (unsigned long)*hdr_len,
                                  (unsigned long)*body_off);

                        return 1;
                }
        }

        debug_log("find_header_end", "header terminator not found");

        return 0;
}

static int valid_status(const char* status) {
        size_t i;
        size_t n;

        debug_log("valid_status", "checking CGI status");

        if (status == NULL) return 0;

        n = strlen(status);

        if (n < 3) return 0;

        if (!ac_is_digit((unsigned char)status[0]) ||
            !ac_is_digit((unsigned char)status[1]) ||
            !ac_is_digit((unsigned char)status[2]))
                return 0;

        if (status[0] < '1' || status[0] > '5') return 0;

        if (n == 3) return 1;

        if (status[3] != ' ') return 0;

        for (i = 4; i < n; i++) {
                if (!ac_is_value((unsigned char)status[i])) return 0;
        }

        return 1;
}

static int valid_response_header(const char* line, size_t len) {
        size_t i;
        size_t colon;

        debug_log("valid_response_header", "checking response header len=%lu",
                  (unsigned long)len);

        if (line == NULL || len == 0) return 0;

        colon = 0;

        while (colon < len && line[colon] != ':') colon++;

        if (colon == 0 || colon == len) return 0;

        for (i = 0; i < colon; i++) {
                if (!ac_is_token((unsigned char)line[i])) return 0;
        }

        for (i = colon + 1; i < len; i++) {
                if (!ac_is_value((unsigned char)line[i])) return 0;
        }

        return 1;
}

static int header_is(const char* line, size_t len, const char* name) {
        size_t name_len;

        if (line == NULL || name == NULL) return 0;

        name_len = strlen(name);

        if (len <= name_len) return 0;

        if (line[name_len] != ':') return 0;

        return strncasecmp(line, name, name_len) == 0;
}

static int is_hop_by_hop_header(const char* line, size_t len) {
        if (header_is(line, len, "Connection") ||
            header_is(line, len, "Proxy-Connection") ||
            header_is(line, len, "Transfer-Encoding") ||
            header_is(line, len, "Keep-Alive") ||
            header_is(line, len, "Upgrade") || header_is(line, len, "TE") ||
            header_is(line, len, "Trailer") ||
            header_is(line, len, "Content-Length")) {
                debug_log(
                    "is_hop_by_hop_header",
                    "header identified as server-controlled framing header");
                return 1;
        }

        return 0;
}

static s4 send_response(int fd, const char* buf, size_t len, int head_only) {
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

        debug_log("send_response", "fd=%d len=%lu head_only=%d", fd,
                  (unsigned long)len, head_only);

        if (fd < 0 || buf == NULL) {
                debug_log("send_response", "invalid arguments");
                return VFS_ERROR;
        }

        if (!find_header_end(buf, len, &hdr_len, &body_off)) {
                debug_log("send_response",
                          "could not find CGI header terminator");
                return VFS_ERROR;
        }

        /*
         * body_off must always be within the received buffer.
         */
        if (body_off > len || hdr_len > body_off) {
                debug_log("send_response",
                          "invalid header/body offsets "
                          "hdr_len=%lu body_off=%lu len=%lu",
                          (unsigned long)hdr_len, (unsigned long)body_off,
                          (unsigned long)len);
                return VFS_ERROR;
        }

        if (hdr_len > SIZE_MAX - 2) {
                debug_log("send_response", "header length overflow hdr_len=%lu",
                          (unsigned long)hdr_len);
                return VFS_ERROR;
        }

        hdrs = (char*)malloc(hdr_len + 2);

        if (hdrs == NULL) {
                debug_log("send_response",
                          "malloc response headers failed size=%lu",
                          (unsigned long)(hdr_len + 2));
                return VFS_ERROR;
        }

        hdrs_len      = 0;
        have_location = 0;

        strcpy(status, "200 OK");
        status_len = strlen(status);

        pos = 0;

        while (pos < hdr_len) {
                eol = pos;

                /*
                 * hdr_len includes the final header's terminating
                 * CRLF/LF, so the LF for every real header line is
                 * inside [pos, hdr_len).
                 */
                while (eol < hdr_len && buf[eol] != '\n') eol++;

                if (eol >= hdr_len) {
                        debug_log("send_response",
                                  "header line has no LF "
                                  "pos=%lu hdr_len=%lu body_off=%lu len=%lu",
                                  (unsigned long)pos, (unsigned long)hdr_len,
                                  (unsigned long)body_off, (unsigned long)len);

                        free(hdrs);
                        return VFS_ERROR;
                }

                line_len = eol - pos;

                /*
                 * Strip CR from CRLF.
                 */
                if (line_len > 0 && buf[pos + line_len - 1] == '\r') line_len--;

                if (line_len == 0) {
                        debug_log("send_response",
                                  "empty CGI response header line "
                                  "pos=%lu eol=%lu",
                                  (unsigned long)pos, (unsigned long)eol);

                        free(hdrs);
                        return VFS_ERROR;
                }

                debug_log("send_response",
                          "processing CGI response header "
                          "pos=%lu eol=%lu line_len=%lu",
                          (unsigned long)pos, (unsigned long)eol,
                          (unsigned long)line_len);

                if (header_is(buf + pos, line_len, "Status")) {
                        size_t s;
                        size_t n;

                        debug_log("send_response", "found CGI Status header");

                        s = pos + strlen("Status") + 1;

                        while (s < pos + line_len &&
                               (buf[s] == ' ' || buf[s] == '\t'))
                                s++;

                        n = pos + line_len - s;

                        if (n == 0 || n >= sizeof(status)) {
                                debug_log("send_response",
                                          "invalid Status length=%lu",
                                          (unsigned long)n);
                                free(hdrs);
                                return VFS_ERROR;
                        }

                        memcpy(status, buf + s, n);
                        status[n] = '\0';

                        debug_log("send_response", "CGI Status value=\"%s\"",
                                  status);

                        if (!valid_status(status)) {
                                debug_log("send_response",
                                          "invalid CGI Status value");
                                free(hdrs);
                                return VFS_ERROR;
                        }

                        status_len = n;

                        debug_log("send_response", "CGI status accepted");
                } else {
                        if (!valid_response_header(buf + pos, line_len)) {
                                debug_log("send_response",
                                          "invalid CGI response header "
                                          "pos=%lu len=%lu",
                                          (unsigned long)pos,
                                          (unsigned long)line_len);
                                free(hdrs);
                                return VFS_ERROR;
                        }

                        if (header_is(buf + pos, line_len, "Location")) {
                                have_location = 1;

                                debug_log("send_response",
                                          "Location header detected");
                        }

                        if (!is_hop_by_hop_header(buf + pos, line_len)) {
                                /*
                                 * Avoid unsigned underflow here.
                                 */
                                if (line_len > hdr_len ||
                                    hdrs_len > hdr_len - line_len ||
                                    hdr_len - line_len - hdrs_len < 2) {
                                        debug_log(
                                            "send_response",
                                            "response header buffer overflow "
                                            "hdrs_len=%lu line_len=%lu "
                                            "hdr_len=%lu",
                                            (unsigned long)hdrs_len,
                                            (unsigned long)line_len,
                                            (unsigned long)hdr_len);

                                        free(hdrs);
                                        return VFS_ERROR;
                                }

                                memcpy(hdrs + hdrs_len, buf + pos, line_len);

                                hdrs_len += line_len;
                                hdrs[hdrs_len++] = '\r';
                                hdrs[hdrs_len++] = '\n';

                                debug_log("send_response",
                                          "copied response header "
                                          "line_len=%lu total_headers=%lu",
                                          (unsigned long)line_len,
                                          (unsigned long)hdrs_len);
                        } else {
                                debug_log("send_response",
                                          "dropping hop-by-hop/framing header");
                        }
                }

                pos = eol + 1;
        }

        if (have_location && strcmp(status, "200 OK") == 0) {
                debug_log("send_response",
                          "Location without explicit Status -> 302 Found");

                strcpy(status, "302 Found");
                status_len = strlen(status);
        }

        body_len = len - body_off;

        debug_log("send_response",
                  "final status=\"%s\" status_len=%lu "
                  "body_len=%lu headers_len=%lu",
                  status, (unsigned long)status_len, (unsigned long)body_len,
                  (unsigned long)hdrs_len);

        if ((size_t)snprintf(head, sizeof(head), "HTTP/1.1 %.*s\r\n",
                             (int)status_len, status) >= sizeof(head)) {
                debug_log("send_response", "HTTP status line too large");

                free(hdrs);
                return VFS_ERROR;
        }

        debug_log("send_response", "writing HTTP status line");

        if (write_all(fd, head, strlen(head)) != 0) goto fail;

        debug_log("send_response", "writing %lu response headers",
                  (unsigned long)hdrs_len);

        if (write_all(fd, hdrs, hdrs_len) != 0) goto fail;

        if ((size_t)snprintf(head, sizeof(head),
                             "Content-Length: %lu\r\n"
                             "Connection: close\r\n"
                             "\r\n",
                             (unsigned long)body_len) >= sizeof(head)) {
                debug_log("send_response",
                          "failed constructing final HTTP headers");
                goto fail;
        }

        debug_log("send_response", "writing generated Content-Length=%lu",
                  (unsigned long)body_len);

        if (write_all(fd, head, strlen(head)) != 0) goto fail;

        if (!head_only) {
                debug_log("send_response", "writing response body len=%lu",
                          (unsigned long)body_len);

                if (write_all(fd, buf + body_off, body_len) != 0) goto fail;
        } else {
                debug_log("send_response",
                          "HEAD request; suppressing response body");
        }

        debug_log("send_response", "response sent successfully");

        free(hdrs);

        return VFS_OK;

fail:
        debug_log("send_response", "failed writing response");

        free(hdrs);

        return VFS_ERROR;
}

/* ------------------------------------------------------------------ */
/* execution helpers                                                  */
/* ------------------------------------------------------------------ */

static int make_deadline(struct timespec* deadline, int seconds) {
        if (deadline == NULL) {
                debug_log("make_deadline", "deadline is NULL");
                return -1;
        }

        if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0) {
                debug_log("make_deadline", "clock_gettime failed errno=%d (%s)",
                          errno, strerror(errno));
                return -1;
        }

        debug_log("make_deadline", "now=%lld.%09ld seconds=%d",
                  (long)deadline->tv_sec, deadline->tv_nsec, seconds);

        deadline->tv_sec += seconds;

        debug_log("make_deadline", "deadline=%lld.%09ld",
                  (long)deadline->tv_sec, deadline->tv_nsec);

        return 0;
}

static int deadline_expired(const struct timespec* deadline) {
        struct timespec now;

        if (deadline == NULL) return 1;

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 1;

        if (now.tv_sec > deadline->tv_sec) return 1;

        if (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)
                return 1;

        return 0;
}

static int deadline_poll_ms(const struct timespec* deadline) {
        struct timespec now;
        time_t sec;
        long nsec;
        long ms;

        if (deadline == NULL) return 0;

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;

        sec  = deadline->tv_sec - now.tv_sec;
        nsec = deadline->tv_nsec - now.tv_nsec;

        if (nsec < 0) {
                sec--;
                nsec += 1000000000L;
        }

        if (sec < 0) return 0;

        ms = (long)sec * 1000LL + (long)(nsec + 999999L) / 1000000LL;

        if (ms > 1000) ms = 1000;

        if (ms < 0) ms = 0;

        return (int)ms;
}

static void kill_and_reap(pid_t pid) {
        int status;

        if (pid <= 0) return;

        debug_log("kill_and_reap", "killing CGI pid=%ld process_group=%ld",
                  (long)pid, (long)-pid);

        (void)kill(-pid, SIGKILL);
        (void)kill(pid, SIGKILL);

        debug_log("kill_and_reap", "waiting for pid=%ld", (long)pid);

        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                debug_log("kill_and_reap", "waitpid interrupted, retrying");
        }

        if (WIFEXITED(status)) {
                debug_log("kill_and_reap", "pid=%ld exited status=%d",
                          (long)pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
                debug_log("kill_and_reap", "pid=%ld killed by signal=%d",
                          (long)pid, WTERMSIG(status));
        } else {
                debug_log("kill_and_reap", "pid=%ld wait status=0x%x",
                          (long)pid, status);
        }
}

/* ------------------------------------------------------------------ */
/* execution                                                          */
/* ------------------------------------------------------------------ */

s4 cgi_run(const struct vfs* vfs, const struct vfs_entry* entry,
           const struct cgi_request* req, int out_fd) {
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

        debug_log("cgi_run", "ENTER vfs=%p entry=%p req=%p out_fd=%d",
                  (void*)vfs, (void*)entry, (void*)req, out_fd);

        if (vfs == NULL || entry == NULL || req == NULL || out_fd < 0) {
                debug_log("cgi_run", "invalid arguments");
                return VFS_ERROR;
        }

        debug_log("cgi_run", "method=%s body_len=%lu header_count=%lu",
                  req->method != NULL ? req->method : "(null)",
                  (unsigned long)req->body_len,
                  (unsigned long)req->header_count);

        if (req->body_len > CGI_MAX_BODY) {
                debug_log("cgi_run", "body too large body_len=%lu max=%lu",
                          (unsigned long)req->body_len,
                          (unsigned long)CGI_MAX_BODY);

                send_error(out_fd, 413, "Request Entity Too Large");
                return VFS_ERROR;
        }

        if (req->body_len > 0 && req->body == NULL) {
                debug_log("cgi_run", "body_len=%lu but body=NULL",
                          (unsigned long)req->body_len);

                send_error(out_fd, 400, "Bad Request");
                return VFS_ERROR;
        }

        head_only = req->method != NULL && strcmp(req->method, "HEAD") == 0;

        debug_log("cgi_run", "head_only=%d", head_only);

        if (disk_path_of(vfs, entry, disk, sizeof(disk)) != VFS_OK) {
                debug_log("cgi_run", "disk_path_of failed");

                send_error(out_fd, 500, "Internal Server Error");
                return VFS_BAD_PATH;
        }

        debug_log("cgi_run", "CGI disk path=%s", disk);

        if (strlen(disk) >= sizeof(dir)) {
                debug_log("cgi_run", "disk path does not fit dir buffer");

                send_error(out_fd, 500, "Internal Server Error");
                return VFS_BAD_PATH;
        }

        strcpy(dir, disk);

        slash = strrchr(dir, '/');

        if (slash == NULL) {
                debug_log("cgi_run", "could not find slash in CGI path");

                send_error(out_fd, 500, "Internal Server Error");
                return VFS_BAD_PATH;
        }

        *slash = '\0';

        debug_log("cgi_run", "CGI working directory=%s", dir);

        env = env_build(entry, req, disk);

        if (env == NULL) {
                debug_log("cgi_run", "env_build failed");

                send_error(out_fd, 500, "Internal Server Error");
                return VFS_ERROR;
        }

        debug_log("cgi_run", "environment built successfully");

        cap = 4096;

        if (cap > CGI_MAX_OUTPUT) cap = CGI_MAX_OUTPUT;

        debug_log("cgi_run", "initial output buffer capacity=%lu max=%lu",
                  (unsigned long)cap, (unsigned long)CGI_MAX_OUTPUT);

        buf = (char*)malloc(cap);

        if (buf == NULL) {
                debug_log("cgi_run", "malloc output buffer failed");

                env_free(env);
                send_error(out_fd, 500, "Internal Server Error");
                return VFS_ERROR;
        }

        if (pipe(in_pipe) != 0) {
                debug_log("cgi_run", "pipe(in_pipe) failed errno=%d (%s)",
                          errno, strerror(errno));

                free(buf);
                env_free(env);
                send_error(out_fd, 500, "Internal Server Error");
                return VFS_ERROR;
        }

        debug_log("cgi_run", "input pipe read=%d write=%d", in_pipe[0],
                  in_pipe[1]);

        if (pipe(out_pipe) != 0) {
                debug_log("cgi_run", "pipe(out_pipe) failed errno=%d (%s)",
                          errno, strerror(errno));

                close(in_pipe[0]);
                close(in_pipe[1]);

                free(buf);
                env_free(env);

                send_error(out_fd, 500, "Internal Server Error");
                return VFS_ERROR;
        }

        debug_log("cgi_run", "output pipe read=%d write=%d", out_pipe[0],
                  out_pipe[1]);

        if (in_pipe[0] <= STDERR_FILENO || in_pipe[1] <= STDERR_FILENO ||
            out_pipe[0] <= STDERR_FILENO || out_pipe[1] <= STDERR_FILENO) {
                debug_log("cgi_run", "pipe descriptor collision with stdio");

                close(in_pipe[0]);
                close(in_pipe[1]);
                close(out_pipe[0]);
                close(out_pipe[1]);

                free(buf);
                env_free(env);

                send_error(out_fd, 500, "Internal Server Error");
                return VFS_ERROR;
        }

        maxfd = sysconf(_SC_OPEN_MAX);

        if (maxfd < 0 || maxfd > 4096) maxfd = 4096;

        debug_log("cgi_run", "child fd cleanup maxfd=%ld", maxfd);

        argv[0] = disk;
        argv[1] = NULL;

        debug_log("cgi_run", "forking CGI executable=%s", disk);

        pid = fork();

        if (pid < 0) {
                debug_log("cgi_run", "fork failed errno=%d (%s)", errno,
                          strerror(errno));

                close(in_pipe[0]);
                close(in_pipe[1]);
                close(out_pipe[0]);
                close(out_pipe[1]);

                free(buf);
                env_free(env);

                send_error(out_fd, 500, "Internal Server Error");
                return VFS_ERROR;
        }

        if (pid == 0) {
                debug_log("cgi_child", "child started pid=%ld", (long)getpid());

                if (setpgid(0, 0) != 0)
                        debug_log("cgi_child", "setpgid failed errno=%d (%s)",
                                  errno, strerror(errno));
                else
                        debug_log("cgi_child", "process group created pgid=%ld",
                                  (long)getpgrp());

                debug_log("cgi_child", "dup2 stdin from fd=%d", in_pipe[0]);

                if (dup2(in_pipe[0], STDIN_FILENO) < 0) {
                        debug_log("cgi_child",
                                  "dup2 stdin failed errno=%d (%s)", errno,
                                  strerror(errno));
                        _exit(126);
                }

                debug_log("cgi_child", "dup2 stdout from fd=%d", out_pipe[1]);

                if (dup2(out_pipe[1], STDOUT_FILENO) < 0) {
                        debug_log("cgi_child",
                                  "dup2 stdout failed errno=%d (%s)", errno,
                                  strerror(errno));
                        _exit(126);
                }

                close(in_pipe[0]);
                close(in_pipe[1]);
                close(out_pipe[0]);
                close(out_pipe[1]);

                debug_log("cgi_child", "closing inherited descriptors >= 3");

                for (fd = 3; fd < maxfd; fd++) close(fd);

                debug_log("cgi_child", "restoring SIGPIPE default");

                signal(SIGPIPE, SIG_DFL);

                debug_log("cgi_child", "chdir(%s)", dir);

                if (chdir(dir) != 0) {
                        debug_log("cgi_child", "chdir failed errno=%d (%s)",
                                  errno, strerror(errno));
                        _exit(126);
                }

                debug_log("cgi_child", "arming child alarm=%d seconds",
                          CGI_TIMEOUT);

                alarm(CGI_TIMEOUT);

                debug_log("cgi_child", "execve(%s)", disk);

                execve(disk, argv, env);

                debug_log("cgi_child", "execve FAILED errno=%d (%s)", errno,
                          strerror(errno));

                _exit(127);
        }

        debug_log("cgi_run", "fork successful child pid=%ld", (long)pid);

        if (setpgid(pid, pid) != 0) {
                debug_log("cgi_run",
                          "parent setpgid(%ld,%ld) failed errno=%d (%s)",
                          (long)pid, (long)pid, errno, strerror(errno));
        } else {
                debug_log("cgi_run", "parent confirmed process group pgid=%ld",
                          (long)pid);
        }

        close(in_pipe[0]);
        close(out_pipe[1]);

        debug_log("cgi_run", "parent closed unused pipe ends");

        env_free(env);

        debug_log("cgi_run", "environment freed in parent");

        sigemptyset(&pipe_set);
        sigaddset(&pipe_set, SIGPIPE);

        debug_log("cgi_run", "blocking SIGPIPE");

        if (pthread_sigmask(SIG_BLOCK, &pipe_set, &old_set) != 0) {
                debug_log("cgi_run", "pthread_sigmask failed");

                close(in_pipe[1]);
                close(out_pipe[0]);

                kill_and_reap(pid);

                free(buf);

                send_error(out_fd, 500, "Internal Server Error");
                return VFS_ERROR;
        }

        debug_log("cgi_run", "SIGPIPE blocked");

        in_fd = in_pipe[1];
        sent  = 0;

        if (req->body_len == 0) {
                debug_log("cgi_run", "no request body; closing CGI stdin");

                close(in_fd);
                in_fd = -1;
        } else {
                flags = fcntl(in_fd, F_GETFL, 0);

                if (flags < 0 ||
                    fcntl(in_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                        debug_log(
                            "cgi_run",
                            "failed making CGI stdin nonblocking errno=%d (%s)",
                            errno, strerror(errno));

                        close(in_fd);
                        close(out_pipe[0]);

                        kill_and_reap(pid);

                        pthread_sigmask(SIG_SETMASK, &old_set, NULL);

                        free(buf);

                        send_error(out_fd, 500, "Internal Server Error");
                        return VFS_ERROR;
                }

                debug_log("cgi_run", "CGI stdin fd=%d is nonblocking", in_fd);
        }

        flags = fcntl(out_pipe[0], F_GETFL, 0);

        if (flags < 0 || fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK) < 0) {
                debug_log("cgi_run",
                          "failed making CGI stdout nonblocking errno=%d (%s)",
                          errno, strerror(errno));

                if (in_fd >= 0) close(in_fd);

                close(out_pipe[0]);

                kill_and_reap(pid);

                pthread_sigmask(SIG_SETMASK, &old_set, NULL);

                free(buf);

                send_error(out_fd, 500, "Internal Server Error");
                return VFS_ERROR;
        }

        debug_log("cgi_run", "CGI stdout fd=%d is nonblocking", out_pipe[0]);

        len          = 0;
        eof          = 0;
        timed_out    = 0;
        output_limit = 0;
        input_error  = 0;

        if (make_deadline(&deadline, CGI_TIMEOUT) != 0) {
                debug_log("cgi_run", "make_deadline failed");

                if (in_fd >= 0) close(in_fd);

                close(out_pipe[0]);

                kill_and_reap(pid);

                pthread_sigmask(SIG_SETMASK, &old_set, NULL);

                free(buf);

                send_error(out_fd, 500, "Internal Server Error");
                return VFS_ERROR;
        }

        debug_log("cgi_run", "entering CGI poll loop");

        for (;;) {
                nf = 0;

                pfd[nf].fd      = out_pipe[0];
                pfd[nf].events  = POLLIN;
                pfd[nf].revents = 0;
                nf++;

                if (in_fd >= 0) {
                        pfd[nf].fd      = in_fd;
                        pfd[nf].events  = POLLOUT;
                        pfd[nf].revents = 0;
                        nf++;
                }

                if (deadline_expired(&deadline)) {
                        debug_log("cgi_run", "deadline expired before poll");

                        timed_out = 1;
                        break;
                }

                {
                        int timeout_ms;

                        timeout_ms = deadline_poll_ms(&deadline);

                        debug_log("cgi_poll",
                                  "poll nfds=%d timeout=%dms stdin_fd=%d "
                                  "stdout_fd=%d sent=%lu/%lu output=%lu",
                                  nf, timeout_ms, in_fd, out_pipe[0],
                                  (unsigned long)sent,
                                  (unsigned long)req->body_len,
                                  (unsigned long)len);

                        poll_rc = poll(pfd, (nfds_t)nf, timeout_ms);
                }

                if (poll_rc < 0) {
                        if (errno == EINTR) {
                                debug_log("cgi_poll",
                                          "poll interrupted by signal");
                                continue;
                        }

                        debug_log("cgi_poll", "poll failed errno=%d (%s)",
                                  errno, strerror(errno));
                        break;
                }

                if (poll_rc == 0) {
                        debug_log("cgi_poll", "poll timeout");

                        timed_out = 1;
                        break;
                }

                debug_log(
                    "cgi_poll",
                    "poll returned=%d stdout_revents=0x%x stdin_revents=0x%x",
                    poll_rc, pfd[0].revents, nf == 2 ? pfd[1].revents : 0);

                if (nf == 2) {
                        if (pfd[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                                debug_log("cgi_input",
                                          "CGI stdin pipe error/hup/nval "
                                          "revents=0x%x",
                                          pfd[1].revents);

                                close(in_fd);
                                in_fd = -1;
                        } else if (pfd[1].revents & POLLOUT) {
                                w = write(in_fd, req->body + sent,
                                          req->body_len - sent);

                                if (w > 0) {
                                        sent += (size_t)w;

                                        debug_log("cgi_input",
                                                  "wrote %ld bytes to CGI "
                                                  "stdin total=%lu/%lu",
                                                  (long)w, (unsigned long)sent,
                                                  (unsigned long)req->body_len);
                                } else if (w < 0 && errno != EAGAIN &&
                                           errno != EINTR) {
                                        debug_log("cgi_input",
                                                  "write to CGI stdin failed "
                                                  "errno=%d (%s)",
                                                  errno, strerror(errno));

                                        input_error = 1;

                                        close(in_fd);
                                        in_fd = -1;
                                } else if (w < 0) {
                                        debug_log("cgi_input",
                                                  "write would "
                                                  "block/interrupted errno=%d",
                                                  errno);
                                }

                                if (in_fd >= 0 && sent >= req->body_len) {
                                        debug_log("cgi_input",
                                                  "entire request body sent; "
                                                  "closing CGI stdin");

                                        close(in_fd);
                                        in_fd = -1;
                                }
                        }
                }

                if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR)) {
                        debug_log("cgi_output",
                                  "CGI stdout readable/hup/error revents=0x%x",
                                  pfd[0].revents);

                        for (;;) {
                                if (len == CGI_MAX_OUTPUT) {
                                        debug_log(
                                            "cgi_output",
                                            "CGI output limit reached=%lu",
                                            (unsigned long)CGI_MAX_OUTPUT);

                                        output_limit = 1;
                                        break;
                                }

                                if (len == cap) {
                                        size_t new_cap;

                                        if (cap >= CGI_MAX_OUTPUT) {
                                                new_cap = CGI_MAX_OUTPUT;
                                        } else if (cap > CGI_MAX_OUTPUT / 2) {
                                                new_cap = CGI_MAX_OUTPUT;
                                        } else {
                                                new_cap = cap * 2;
                                        }

                                        debug_log(
                                            "cgi_output",
                                            "growing output buffer %lu -> %lu",
                                            (unsigned long)cap,
                                            (unsigned long)new_cap);

                                        tmp = (char*)realloc(buf, new_cap);

                                        if (tmp == NULL) {
                                                debug_log(
                                                    "cgi_output",
                                                    "realloc failed "
                                                    "new_cap=%lu",
                                                    (unsigned long)new_cap);
                                                break;
                                        }

                                        buf = tmp;
                                        cap = new_cap;
                                }

                                r = read(out_pipe[0], buf + len, cap - len);

                                if (r > 0) {
                                        len += (size_t)r;

                                        debug_log(
                                            "cgi_output",
                                            "read %ld bytes total_output=%lu "
                                            "capacity=%lu",
                                            (long)r, (unsigned long)len,
                                            (unsigned long)cap);

                                        continue;
                                }

                                if (r == 0) {
                                        debug_log("cgi_output",
                                                  "CGI stdout EOF");

                                        eof = 1;
                                        break;
                                }

                                if (errno == EINTR) {
                                        debug_log("cgi_output",
                                                  "read interrupted, retrying");
                                        continue;
                                }

                                if (errno == EAGAIN) {
                                        debug_log("cgi_output",
                                                  "stdout drained for now");
                                        break;
                                }

                                debug_log("cgi_output",
                                          "read failed errno=%d (%s)", errno,
                                          strerror(errno));
                                break;
                        }

                        if (output_limit || eof) break;
                }
        }

        debug_log("cgi_run",
                  "leaving poll loop eof=%d timed_out=%d output_limit=%d "
                  "input_error=%d output_len=%lu sent=%lu/%lu",
                  eof, timed_out, output_limit, input_error, (unsigned long)len,
                  (unsigned long)sent, (unsigned long)req->body_len);

        if (in_fd >= 0) {
                debug_log("cgi_run", "closing CGI stdin fd=%d", in_fd);
                close(in_fd);
        }

        debug_log("cgi_run", "closing CGI stdout fd=%d", out_pipe[0]);

        close(out_pipe[0]);

        if (!eof) {
                debug_log("cgi_run", "CGI did not reach EOF; killing process");

                kill_and_reap(pid);
        } else {
                debug_log("cgi_run",
                          "CGI reached EOF; waiting for child pid=%ld",
                          (long)pid);

                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                        debug_log("cgi_run", "waitpid interrupted");
                }

                if (WIFEXITED(status)) {
                        debug_log("cgi_run", "CGI exited normally status=%d",
                                  WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                        debug_log("cgi_run", "CGI terminated by signal=%d",
                                  WTERMSIG(status));
                } else {
                        debug_log("cgi_run", "CGI wait status=0x%x", status);
                }
        }

        if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE)) {
                debug_log("cgi_run", "SIGPIPE pending; consuming it");

                zero.tv_sec  = 0;
                zero.tv_nsec = 0;

                (void)sigtimedwait(&pipe_set, NULL, &zero);
        }

        debug_log("cgi_run", "restoring original signal mask");

        pthread_sigmask(SIG_SETMASK, &old_set, NULL);

        if (timed_out) {
                debug_log("cgi_run", "RESULT: CGI TIMEOUT");

                free(buf);

                send_error(out_fd, 504, "Gateway Timeout");

                return VFS_ERROR;
        }

        if (output_limit) {
                debug_log("cgi_run", "RESULT: CGI OUTPUT TOO LARGE");

                free(buf);

                send_error(out_fd, 502, "CGI Output Too Large");

                return VFS_ERROR;
        }

        if (!eof) {
                debug_log("cgi_run", "RESULT: CGI DID NOT EOF");

                free(buf);

                send_error(out_fd, 502, "Bad Gateway");

                return VFS_ERROR;
        }

        if (input_error && len == 0) {
                debug_log("cgi_run", "RESULT: CGI INPUT ERROR WITH NO OUTPUT");

                free(buf);

                send_error(out_fd, 502, "Bad Gateway");

                return VFS_ERROR;
        }

        if (len == 0) {
                debug_log("cgi_run", "CGI produced zero output");

                free(buf);

                if (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM) {
                        debug_log("cgi_run", "CGI died from SIGALRM");

                        send_error(out_fd, 504, "Gateway Timeout");
                } else {
                        debug_log("cgi_run", "CGI failed without output");

                        send_error(out_fd, 502, "Bad Gateway");
                }

                return VFS_ERROR;
        }

        debug_log("cgi_run", "passing %lu CGI output bytes to send_response",
                  (unsigned long)len);

        if (send_response(out_fd, buf, len, head_only) != VFS_OK) {
                debug_log("cgi_run", "send_response failed");

                free(buf);

                send_error(out_fd, 502, "Bad Gateway");

                return VFS_ERROR;
        }

        debug_log("cgi_run", "CGI request completed successfully");

        free(buf);

        return VFS_OK;
}
