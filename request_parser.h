/* request_parser.h - HTTP/1.x request parser.
 *
 * Parses the request line and header fields from one complete HTTP request.
 * This module does not access the filesystem and does not interpret the
 * request-target as a filesystem path.
 *
 * The parser is deliberately bounded:
 *   - maximum request-header block
 *   - maximum request-target
 *   - maximum header count
 *   - maximum header name/value
 *
 * C89 compatible.
 */
#ifndef REQUEST_PARSER_H
#define REQUEST_PARSER_H

#include <stddef.h>

#include "lib.h"

/* Hard parser limits. Keep these comfortably below any server buffer. */
#define REQUEST_MAX_HEADERS       64
#define REQUEST_MAX_TARGET        4096
#define REQUEST_MAX_HEADER_NAME   256
#define REQUEST_MAX_HEADER_VALUE  8192
#define REQUEST_MAX_HEADER_BYTES  16384

#define REQUEST_METHOD_GET        1
#define REQUEST_METHOD_HEAD       2

#define REQUEST_HTTP_10           10
#define REQUEST_HTTP_11           11

#define REQUEST_PARSE_OK          0
#define REQUEST_PARSE_INCOMPLETE  1
#define REQUEST_PARSE_BAD         (-1)
#define REQUEST_PARSE_TOO_LARGE   (-2)

/*
 * One header field.
 *
 * name is stored in its original spelling.
 * Header lookup should be case-insensitive.
 *
 * value has surrounding optional whitespace removed.
 */
struct request_header {
    char name[REQUEST_MAX_HEADER_NAME];
    char value[REQUEST_MAX_HEADER_VALUE];
};

struct http_request {
    s4 method;                         /* REQUEST_METHOD_* */
    s4 version;                        /* REQUEST_HTTP_* */

    char target[REQUEST_MAX_TARGET];

    struct request_header
        headers[REQUEST_MAX_HEADERS];
    s4 header_count;

    /*
     * Content-Length, if present.
     *
     * has_content_length == 0:
     *     no Content-Length header was supplied.
     *
     * has_content_length == 1:
     *     content_length contains the parsed value.
     */
    s4 has_content_length;
    u8 content_length;

    /*
     * Number of bytes consumed by the complete request header,
     * including the final CRLF CRLF.
     */
    size_t header_bytes;
};

/*
 * Parse one HTTP request header block.
 *
 * `buf` does not need to be NUL terminated.
 * `len` is the number of bytes currently available.
 *
 * On REQUEST_PARSE_OK:
 *   - req is filled in
 *   - *consumed contains the number of bytes belonging to the header block
 *
 * If the buffer does not yet contain CRLF CRLF:
 *   REQUEST_PARSE_INCOMPLETE
 *
 * If syntax is invalid:
 *   REQUEST_PARSE_BAD
 *
 * If one of the configured limits is exceeded:
 *   REQUEST_PARSE_TOO_LARGE
 *
 * The parser never reads past `len`.
 */
s4 request_parse(const char *buf, size_t len,
                 struct http_request *req, size_t *consumed);

/* Case-insensitive header lookup. Returns NULL if absent. */
const struct request_header *
request_header_get(const struct http_request *req, const char *name);

#endif
