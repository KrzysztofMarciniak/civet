/* request_parser.c - bounded HTTP/1.x request parser. */

#include "request_parser.h"
#include "allowed_chars.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static s4 is_space(u1 c)
{
    return c == (u1)' ' || c == (u1)'\t';
}

static s4 ascii_lower(u1 c)
{
    if (c >= (u1)'A' && c <= (u1)'Z')
        return (s4)(c + ((u1)'a' - (u1)'A'));

    return (s4)c;
}

/*
 * HTTP header field names are ASCII tokens. Comparison is therefore
 * deliberately ASCII-only rather than depending on the current locale.
 */
static s4 name_equal(const char *a, const char *b)
{
    u1 ca;
    u1 cb;

    while (*a != '\0' && *b != '\0') {
        ca = (u1)*a;
        cb = (u1)*b;

        if (ascii_lower(ca) != ascii_lower(cb))
            return 0;

        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

static s4 is_crlf_at(const char *buf, size_t len, size_t p)
{
    if (p + 1 >= len)
        return 0;

    return (u1)buf[p] == (u1)'\r' &&
           (u1)buf[p + 1] == (u1)'\n';
}

/*
 * Find CRLF starting at `start`.
 *
 * Returns 1 when found and writes its position to *end.
 * Returns 0 when the line is not yet complete.
 */
static s4 find_crlf(const char *buf, size_t len,
                    size_t start, size_t *end)
{
    size_t p;

    for (p = start; p + 1 < len; p++) {
        if ((u1)buf[p] == (u1)'\r' &&
            (u1)buf[p + 1] == (u1)'\n') {
            *end = p;
            return 1;
        }

        /*
         * Bare CR or LF is never a valid line ending. If either is
         * encountered before CRLF, the request is malformed rather
         * than merely incomplete.
         */
        if ((u1)buf[p] == (u1)'\r' ||
            (u1)buf[p] == (u1)'\n') {
            return -1;
        }
    }

    return 0;
}

/*
 * Copy [start,end) into dst and append NUL.
 */
static s4 copy_bytes(char *dst, size_t dst_size,
                     const char *buf, size_t start, size_t end)
{
    size_t n;

    if (end < start)
        return -1;

    n = end - start;

    if (n + 1 > dst_size)
        return -1;

    if (n != 0)
        memcpy(dst, buf + start, n);

    dst[n] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* request line                                                        */
/* ------------------------------------------------------------------ */

/*
 * Parse:
 *
 *     METHOD SP request-target SP HTTP-version CRLF
 *
 * No other whitespace is accepted in the request line.
 */
static s4 parse_request_line(const char *buf,
                             size_t start,
                             size_t end,
                             struct http_request *req)
{
    size_t p;
    size_t method_start;
    size_t method_end;
    size_t target_start;
    size_t target_end;
    size_t version_start;
    size_t i;
    size_t n;

    method_start = start;
    p = start;

    /*
     * Method.
     */
    while (p < end && (u1)buf[p] != (u1)' ')
        p++;

    if (p == end)
        return REQUEST_PARSE_BAD;

    method_end = p;

    if (method_end == method_start)
        return REQUEST_PARSE_BAD;

    /*
     * Every method character must be tchar.
     */
    for (i = method_start; i < method_end; i++) {
        if (!ac_is_token((u1)buf[i]))
            return REQUEST_PARSE_BAD;
    }

    /*
     * We intentionally only implement GET and HEAD.
     */
    n = method_end - method_start;

    if (n == 3 &&
        buf[method_start] == 'G' &&
        buf[method_start + 1] == 'E' &&
        buf[method_start + 2] == 'T') {
        req->method = REQUEST_METHOD_GET;
    } else if (n == 4 &&
               buf[method_start] == 'H' &&
               buf[method_start + 1] == 'E' &&
               buf[method_start + 2] == 'A' &&
               buf[method_start + 3] == 'D') {
        req->method = REQUEST_METHOD_HEAD;
    } else {
        return REQUEST_PARSE_BAD;
    }

    /*
     * Exactly one SP between method and target.
     */
    p++;

    target_start = p;

    while (p < end && (u1)buf[p] != (u1)' ')
        p++;

    if (p == end)
        return REQUEST_PARSE_BAD;

    target_end = p;

    if (target_end == target_start)
        return REQUEST_PARSE_BAD;

    /*
     * Request-target must contain URI-allowed bytes according to our
     * table. This also means NUL and all other control bytes fail.
     */
    for (i = target_start; i < target_end; i++) {
        if (!ac_is_uri((u1)buf[i]))
            return REQUEST_PARSE_BAD;
    }

    /*
     * This server serves origin-form targets:
     *
     *     /index.html
     *     /foo/bar?x=1
     *
     * Absolute-form is useful for proxies but is unnecessary here.
     */
    if (buf[target_start] != '/')
        return REQUEST_PARSE_BAD;

    if (copy_bytes(req->target, sizeof(req->target),
                   buf, target_start, target_end) != 0)
        return REQUEST_PARSE_TOO_LARGE;

    /*
     * Exactly one SP between target and HTTP-version.
     */
    p++;

    version_start = p;

    while (p < end && (u1)buf[p] != (u1)' ')
        p++;

    /*
     * There must not be another space after HTTP-version.
     *
     * Since this is the request-line itself, the final byte before CRLF
     * is the end of the version.
     */
    if (p != end)
        return REQUEST_PARSE_BAD;

    if (end - version_start == 8 &&
        memcmp(buf + version_start, "HTTP/1.0", 8) == 0) {
        req->version = REQUEST_HTTP_10;
    } else if (end - version_start == 8 &&
               memcmp(buf + version_start, "HTTP/1.1", 8) == 0) {
        req->version = REQUEST_HTTP_11;
    } else {
        return REQUEST_PARSE_BAD;
    }

    return REQUEST_PARSE_OK;
}

/* ------------------------------------------------------------------ */
/* Content-Length                                                      */
/* ------------------------------------------------------------------ */

/*
 * Parse an unsigned decimal Content-Length.
 *
 * No signs, whitespace, hexadecimal, or other syntax are accepted.
 */
static s4 parse_content_length(const char *value, u8 *out)
{
    const unsigned char *p;
    u8 n;
    u1 digit;

    if (value == NULL || *value == '\0')
        return -1;

    p = (const unsigned char *)value;
    n = 0;

    while (*p != '\0') {
        if (!ac_is_digit(*p))
            return -1;

        digit = (u1)(*p - (unsigned char)'0');

        /*
         * Overflow test:
         *
         * n * 10 + digit <= U8_MAX
         *
         * U8_MAX is not available in the current lib.h, so use the
         * maximum value representable by u8.
         */
        if (n > (u8)(~(u8)0) / (u8)10)
            return -1;

        if (n == (u8)(~(u8)0) / (u8)10 &&
            (u8)digit > (u8)(~(u8)0) % (u8)10)
            return -1;

        n = n * (u8)10 + (u8)digit;
        p++;
    }

    *out = n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* header parsing                                                      */
/* ------------------------------------------------------------------ */

/*
 * Parse one header line:
 *
 *     field-name ":" OWS field-value OWS
 *
 * `start` and `end` delimit the bytes before CRLF.
 */
static s4 parse_header_line(const char *buf,
                            size_t start,
                            size_t end,
                            struct http_request *req)
{
    size_t p;
    size_t colon;
    size_t value_start;
    size_t value_end;
    struct request_header *h;

    if (start == end)
        return REQUEST_PARSE_OK;

    /*
     * Leading whitespace would be obsolete line folding. Reject it
     * rather than treating it as part of the previous header.
     */
    if (is_space((u1)buf[start]))
        return REQUEST_PARSE_BAD;

    colon = start;

    while (colon < end && (u1)buf[colon] != (u1)':')
        colon++;

    if (colon == end)
        return REQUEST_PARSE_BAD;

    /*
     * Field-name must be non-empty and entirely tchar.
     */
    if (colon == start)
        return REQUEST_PARSE_BAD;

    for (p = start; p < colon; p++) {
        if (!ac_is_token((u1)buf[p]))
            return REQUEST_PARSE_BAD;
    }

    if (req->header_count >= REQUEST_MAX_HEADERS)
        return REQUEST_PARSE_TOO_LARGE;

    h = &req->headers[req->header_count];

    if (copy_bytes(h->name, sizeof(h->name),
                   buf, start, colon) != 0)
        return REQUEST_PARSE_TOO_LARGE;

    /*
     * Skip the colon and optional whitespace.
     */
    value_start = colon + 1;

    while (value_start < end &&
           is_space((u1)buf[value_start]))
        value_start++;

    /*
     * Remove trailing OWS.
     */
    value_end = end;

    while (value_end > value_start &&
           is_space((u1)buf[value_end - 1]))
        value_end--;

    /*
     * Every remaining byte must be permitted in a field value.
     *
     * In particular this rejects:
     *   NUL
     *   bare CR/LF
     *   other controls
     *   DEL
     *
     * obs-text 0x80..0xFF is permitted by AC_VALUE.
     */
    for (p = value_start; p < value_end; p++) {
        if (!ac_is_value((u1)buf[p]))
            return REQUEST_PARSE_BAD;
    }

    if (copy_bytes(h->value, sizeof(h->value),
                   buf, value_start, value_end) != 0)
        return REQUEST_PARSE_TOO_LARGE;

    req->header_count++;

    /*
     * Process headers whose semantics matter to the parser.
     */
    if (name_equal(h->name, "Content-Length")) {
        u8 value;

        /*
         * Multiple Content-Length fields are deliberately rejected.
         * That avoids having to implement the several equivalent-value
         * cases and keeps request framing unambiguous.
         */
        if (req->has_content_length)
            return REQUEST_PARSE_BAD;

        if (parse_content_length(h->value, &value) != 0)
            return REQUEST_PARSE_BAD;

        req->has_content_length = 1;
        req->content_length = value;
    } else if (name_equal(h->name, "Transfer-Encoding")) {
        /*
         * We do not implement HTTP message transfer codings.
         * Rejecting the header entirely avoids request-smuggling
         * ambiguity between this parser and the socket reader.
         */
        return REQUEST_PARSE_BAD;
    }

    return REQUEST_PARSE_OK;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

const struct request_header *
request_header_get(const struct http_request *req, const char *name)
{
    s4 i;

    if (req == NULL || name == NULL)
        return NULL;

    for (i = 0; i < req->header_count; i++) {
        if (name_equal(req->headers[i].name, name))
            return &req->headers[i];
    }

    return NULL;
}

s4 request_parse(const char *buf, size_t len,
                 struct http_request *req, size_t *consumed)
{
    size_t p;
    size_t line_end;
    s4 r;
    const struct request_header *host;

    if (buf == NULL || req == NULL || consumed == NULL)
        return REQUEST_PARSE_BAD;

    *consumed = 0;

    if (len == 0)
        return REQUEST_PARSE_INCOMPLETE;

    /*
     * A request header block larger than this is never accepted.
     */
    if (len > REQUEST_MAX_HEADER_BYTES)
        len = REQUEST_MAX_HEADER_BYTES;

    memset(req, 0, sizeof(*req));

    /*
     * ---------------- request line ----------------
     */
    r = find_crlf(buf, len, 0, &line_end);

    if (r == 0)
        return REQUEST_PARSE_INCOMPLETE;

    if (r < 0)
        return REQUEST_PARSE_BAD;

    r = parse_request_line(buf, 0, line_end, req);

    if (r != REQUEST_PARSE_OK)
        return r;

    p = line_end + 2;

    /*
     * ---------------- headers ----------------
     *
     * The empty line CRLF terminates the header block.
     */
    for (;;) {
        size_t header_end;

        r = find_crlf(buf, len, p, &header_end);

        if (r == 0)
            return REQUEST_PARSE_INCOMPLETE;

        if (r < 0)
            return REQUEST_PARSE_BAD;

        /*
         * Empty line: end of headers.
         */
        if (header_end == p) {
            p += 2;
            break;
        }

        r = parse_header_line(buf, p, header_end, req);

        if (r != REQUEST_PARSE_OK)
            return r;

        p = header_end + 2;

        if (p > REQUEST_MAX_HEADER_BYTES)
            return REQUEST_PARSE_TOO_LARGE;
    }

    /*
     * HTTP/1.1 requires Host.
     */
    if (req->version == REQUEST_HTTP_11) {
        host = request_header_get(req, "Host");

        if (host == NULL)
            return REQUEST_PARSE_BAD;

        /*
         * An empty Host value is not useful to this server.
         */
        if (host->value[0] == '\0')
            return REQUEST_PARSE_BAD;
    }

    req->header_bytes = p;
    *consumed = p;

    return REQUEST_PARSE_OK;
}
