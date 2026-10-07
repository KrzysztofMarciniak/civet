/* allowed_chars.h - which bytes may appear where in an HTTP request.
 *
 * One 256-entry table of class bits. The table is filled by ac_init()
 * from the readable definitions in allowed_chars.c. If ac_init() is
 * forgotten the table is all zero, so every check fails: closed by default.
 */
#ifndef ALLOWED_CHARS_H
#define ALLOWED_CHARS_H

#include "lib.h"

#define AC_TOKEN  0x01  /* method, header name (RFC 9110 tchar)       */
#define AC_URI    0x02  /* request-target; '#' excluded, '%' included */
#define AC_VALUE  0x04  /* header field value: VCHAR SP HTAB obs-text */
#define AC_DIGIT  0x08
#define AC_HEX    0x10

extern u1 ac_table[256];

/* Fill the table. Call once at startup, before any request is parsed. */
void ac_init(void);

/* The cast to u1 makes these safe for negative plain chars. */
#define ac_has(c, cls)   (ac_table[(u1)(c)] & (cls))
#define ac_is_token(c)   ac_has((c), AC_TOKEN)
#define ac_is_uri(c)     ac_has((c), AC_URI)
#define ac_is_value(c)   ac_has((c), AC_VALUE)
#define ac_is_digit(c)   ac_has((c), AC_DIGIT)
#define ac_is_hex(c)     ac_has((c), AC_HEX)

#endif
