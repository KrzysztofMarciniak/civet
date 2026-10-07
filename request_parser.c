/* request_parser.c - bounded HTTP/1.x request parser. */

#include "request_parser.h"
#include "allowed_chars.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static s4 is_ows(u1 c)
{
    return c == (u1)' ' || c == (u1)'\t';
}

static s4 ascii_lower(u1 c)
{
    if (c >= (u1)'A' && c <= (u1)'Z')
        return (s4)(c + ((u1)'a' - (u1)'A'));

    return (s4)c;
}

static s4 name_equal(const char *a,
                     size_t a_len,
                     const char *b)
{
    size_t i;
    u1 ca;
    u1 cb;

    if (a == NULL || b == NULL)
        return 0;

    for (i = 0; i < a_len; i++) {
        if (b[i] == '\0')
            return 0;

        ca = (u1)a[i];
        cb = (u1)b[i];

        if (ascii_lower(ca) != ascii_lower(cb))
            return 0;
    }

    return b[a_len] == '\0';
}

/*
 * Find CRLF starting at `start`.
 *
 * Returns:
 *   1  CRLF found
 *   0  incomplete
 *  -1  malformed bare CR/LF found
 */
static s4 find_crlf(const char *buf,
                    size_t len,
                    size_t start,
                    size_t *end)
{
    size_t p;

    for (p = start; p < len; p++) {
        if ((u1)buf[p] == (u1)'\r') {
            if (p + 1 >= len)
                return 0;

            if ((u1)buf[p + 1] != (u1)'\n')
                return -1;

            *end = p;
            return 1;
        }

        if ((u1)buf[p] == (u1)'\n')
            return -1;
    }

    return 0;
}

/*
 * Validate origin-form percent escapes.
 *
 * '%' must always be followed by two hexadecimal digits.
 */
static s4 valid_percent_encoding(const char *buf,
                                 size_t start,
                                 size_t end)
{
    size_t p;

    for (p = start; p < end; p++) {
        if ((u1)buf[p] != (u1)'%')
            continue;

        if (p + 2 >= end)
            return 0;

        if (!ac_is_hex((u1)buf[p + 1]) ||
            !ac_is_hex((u1)buf[p + 2]))
            return 0;

        p += 2;
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/* request line                                                        */
/* ------------------------------------------------------------------ */

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
     * METHOD
     */
    while (p < end && (u1)buf[p] != (u1)' ')
        p++;

    if (p == end)
        return REQUEST_PARSE_BAD;

    method_end = p;

    if (method_end == method_start)
        return REQUEST_PARSE_BAD;

    for (i = method_start; i < method_end; i++) {
        if (!ac_is_token((u1)buf[i]))
            return REQUEST_PARSE_BAD;
    }

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
     * Exactly one SP.
     */
    p++;

    if (p >= end || (u1)buf[p] == (u1)' ')
        return REQUEST_PARSE_BAD;

    target_start = p;

    while (p < end && (u1)buf[p] != (u1)' ')
        p++;

    if (p == end)
        return REQUEST_PARSE_BAD;

    target_end = p;

    if (target_end == target_start)
        return REQUEST_PARSE_BAD;

    /*
     * Origin-form only.
     */
    if (buf[target_start] != '/')
        return REQUEST_PARSE_BAD;

    /*
     * Validate request-target characters.
     */
    for (i = target_start; i < target_end; i++) {
        if (!ac_is_uri((u1)buf[i]))
            return REQUEST_PARSE_BAD;
    }

    /*
     * '%' is permitted in URI syntax but must form a valid
     * percent-encoded triplet.
     */
    if (!valid_percent_encoding(buf,
                                target_start,
                                target_end))
        return REQUEST_PARSE_BAD;

    if (target_end - target_start + 1 >
        sizeof(req->target))
        return REQUEST_PARSE_TOO_LARGE;

    memcpy(req->target,
           buf + target_start,
           target_end - target_start);

    req->target[target_end - target_start] = '\0';

    /*
     * Exactly one SP before HTTP-version.
     */
    p++;

    if (p >= end || (u1)buf[p] == (u1)' ')
        return REQUEST_PARSE_BAD;

    version_start = p;

    /*
     * There cannot be another SP in HTTP-version.
     */
    while (p < end && (u1)buf[p] != (u1)' ')
        p++;

    if (p != end)
        return REQUEST_PARSE_BAD;

    if (end - version_start == 8 &&
        memcmp(buf + version_start,
               "HTTP/1.0",
               8) == 0) {
        req->version = REQUEST_HTTP_10;
    } else if (end - version_start == 8 &&
               memcmp(buf + version_start,
                      "HTTP/1.1",
                      8) == 0) {
        req->version = REQUEST_HTTP_11;
    } else {
        return REQUEST_PARSE_BAD;
    }

    return REQUEST_PARSE_OK;
}

/* ------------------------------------------------------------------ */
/* Content-Length                                                      */
/* ------------------------------------------------------------------ */

static s4 parse_content_length(const char *value,
                               size_t len,
                               u8 *out)
{
    size_t p;
    u8 n;
    u1 digit;
    u8 max_value;

    if (value == NULL || out == NULL || len == 0)
        return -1;

    n = 0;
    max_value = (u8)~(u8)0;

    for (p = 0; p < len; p++) {
        if (!ac_is_digit((u1)value[p]))
            return -1;

        digit = (u1)(value[p] - '0');

        if (n > max_value / (u8)10)
            return -1;

        if (n == max_value / (u8)10 &&
            (u8)digit > max_value % (u8)10)
            return -1;

        n = n * (u8)10 + (u8)digit;
    }

    *out = n;

    return 0;
}

/* ------------------------------------------------------------------ */
/* header parsing                                                      */
/* ------------------------------------------------------------------ */

static s4 parse_header_line(const char *buf,
                            size_t start,
                            size_t end,
                            struct http_request *req)
{
    size_t p;
    size_t colon;
    size_t value_start;
    size_t value_end;
    size_t name_len;
    size_t value_len;
    struct request_header *h;

    if (start == end)
        return REQUEST_PARSE_OK;

    /*
     * Reject obs-fold / leading whitespace.
     */
    if (is_ows((u1)buf[start]))
        return REQUEST_PARSE_BAD;

    colon = start;

    while (colon < end &&
           (u1)buf[colon] != (u1)':')
        colon++;

    if (colon == end)
        return REQUEST_PARSE_BAD;

    if (colon == start)
        return REQUEST_PARSE_BAD;

    /*
     * No whitespace is permitted before ':'.
     */
    if (is_ows((u1)buf[colon - 1]))
        return REQUEST_PARSE_BAD;

    /*
     * Field-name = token.
     */
    for (p = start; p < colon; p++) {
        if (!ac_is_token((u1)buf[p]))
            return REQUEST_PARSE_BAD;
    }

    if (req->header_count >= REQUEST_MAX_HEADERS)
        return REQUEST_PARSE_TOO_LARGE;

    name_len = colon - start;

    if (name_len > REQUEST_MAX_HEADER_NAME)
        return REQUEST_PARSE_TOO_LARGE;

    /*
     * Skip ':' and leading OWS.
     */
    value_start = colon + 1;

    while (value_start < end &&
           is_ows((u1)buf[value_start]))
        value_start++;

    /*
     * Remove trailing OWS.
     */
    value_end = end;

    while (value_end > value_start &&
           is_ows((u1)buf[value_end - 1]))
        value_end--;

    value_len = value_end - value_start;

    if (value_len > REQUEST_MAX_HEADER_VALUE)
        return REQUEST_PARSE_TOO_LARGE;

    /*
     * Field-value:
     *
     *   HTAB
     *   SP
     *   VCHAR
     *   obs-text
     *
     * Reject all other control characters, including DEL and NUL.
     */
    for (p = value_start; p < value_end; p++) {
        u1 c;

        c = (u1)buf[p];

        if (c == 0x7F)
            return REQUEST_PARSE_BAD;

        if (!ac_is_value(c))
            return REQUEST_PARSE_BAD;
    }

    h = &req->headers[req->header_count];

    h->name = buf + start;
    h->name_len = name_len;

    h->value = buf + value_start;
    h->value_len = value_len;

    req->header_count++;

    /*
     * Content-Length.
     */
    if (name_equal(h->name,
                   h->name_len,
                   "Content-Length")) {
        u8 value;

        /*
         * This server does not consume request bodies.
         * Therefore a non-zero Content-Length is rejected.
         */
        if (req->has_content_length)
            return REQUEST_PARSE_BAD;

        if (parse_content_length(h->value,
                                 h->value_len,
                                 &value) != 0)
            return REQUEST_PARSE_BAD;

        if (value != (u8)0)
            return REQUEST_PARSE_BAD;

        req->has_content_length = 1;
        req->content_length = value;
    }

    /*
     * Transfer-Encoding is not implemented by Civet.
     */
    else if (name_equal(h->name,
                        h->name_len,
                        "Transfer-Encoding")) {
        if (req->has_transfer_encoding)
            return REQUEST_PARSE_BAD;

        req->has_transfer_encoding = 1;

        return REQUEST_PARSE_BAD;
    }

    return REQUEST_PARSE_OK;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

const struct request_header *
request_header_get(const struct http_request *req,
                   const char *name)
{
    s4 i;

    if (req == NULL || name == NULL)
        return NULL;

    for (i = 0; i < req->header_count; i++) {
        if (name_equal(req->headers[i].name,
                       req->headers[i].name_len,
                       name))
            return &req->headers[i];
    }

    return NULL;
}

s4 request_parse(const char *buf,
                 size_t len,
                 struct http_request *req,
                 size_t *consumed)
{
    size_t p;
    size_t line_end;
    size_t header_end;
    s4 r;
    const struct request_header *host;

    if (buf == NULL ||
        req == NULL ||
        consumed == NULL)
        return REQUEST_PARSE_BAD;

    *consumed = 0;

    if (len == 0)
        return REQUEST_PARSE_INCOMPLETE;

    /*
     * Never silently truncate an oversized header buffer.
     */
    if (len > REQUEST_MAX_HEADER_BYTES)
        return REQUEST_PARSE_TOO_LARGE;

    memset(req, 0, sizeof(*req));

    /*
     * Request line.
     */
    r = find_crlf(buf,
                  len,
                  0,
                  &line_end);

    if (r == 0)
        return REQUEST_PARSE_INCOMPLETE;

    if (r < 0)
        return REQUEST_PARSE_BAD;

    r = parse_request_line(buf,
                           0,
                           line_end,
                           req);

    if (r != REQUEST_PARSE_OK)
        return r;

    p = line_end + 2;

    /*
     * Header section.
     */
    for (;;) {
        r = find_crlf(buf,
                      len,
                      p,
                      &header_end);

        if (r == 0)
            return REQUEST_PARSE_INCOMPLETE;

        if (r < 0)
            return REQUEST_PARSE_BAD;

        /*
         * Empty line terminates the header section.
         */
        if (header_end == p) {
            p += 2;
            break;
        }

        r = parse_header_line(buf,
                              p,
                              header_end,
                              req);

        if (r != REQUEST_PARSE_OK)
            return r;

        p = header_end + 2;

        if (p > REQUEST_MAX_HEADER_BYTES)
            return REQUEST_PARSE_TOO_LARGE;
    }

    /*
     * Host is mandatory for HTTP/1.1.
     */
    if (req->version == REQUEST_HTTP_11) {
        host = request_header_get(req, "Host");

        if (host == NULL)
            return REQUEST_PARSE_BAD;

        /*
         * Keep Civet's policy that Host must not be empty.
         */
        if (host->value_len == 0)
            return REQUEST_PARSE_BAD;

        /*
         * Host is a singleton field.
         */
        {
            s4 i;
            s4 host_count;

            host_count = 0;

            for (i = 0; i < req->header_count; i++) {
                if (name_equal(req->headers[i].name,
                               req->headers[i].name_len,
                               "Host")) {
                    host_count++;
                }
            }

            if (host_count != 1)
                return REQUEST_PARSE_BAD;
        }
    }

    /*
     * No request body is supported.
     *
     * A Content-Length of zero is harmless; anything else was
     * rejected while parsing the header.
     */
    req->header_bytes = p;
    *consumed = p;

    return REQUEST_PARSE_OK;
}
