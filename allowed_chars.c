/* allowed_chars.c - the single place that defines allowed characters. */
#include "allowed_chars.h"

u1 ac_table[256];

/* ---- definitions: edit these strings to change what the server accepts ---- */

#define ALPHA "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define DIGIT "0123456789"

/* RFC 9110 tchar: ALPHA / DIGIT / these */
static const char token_extra[] = "!#$%&'*+-.^_`|~";

/* RFC 3986 unreserved + reserved + '%', minus '#' (never sent by clients) */
static const char uri_extra[] = "-._~" ":/?[]@" "!$&'()*+,;=" "%";

static const char hex_chars[] = "0123456789abcdefABCDEF";

/* ---- implementation ---- */

static void add(const char *chars, u1 cls)
{
    for (; *chars != '\0'; chars++)
        ac_table[(u1)*chars] |= cls;
}

void ac_init(void)
{
    s4 c;

    for (c = 0; c < 256; c++)
        ac_table[c] = 0;

    add(ALPHA DIGIT, AC_TOKEN | AC_URI);
    add(token_extra, AC_TOKEN);
    add(uri_extra, AC_URI);
    add(DIGIT, AC_DIGIT);
    add(hex_chars, AC_HEX);

    /* header values: SP, HTAB, visible ASCII, and obs-text (0x80-0xFF);
     * control characters and DEL (0x7F) are rejected. */
    ac_table[(u1)'\t'] |= AC_VALUE;
    for (c = 0x20; c <= 0x7E; c++)
        ac_table[c] |= AC_VALUE;
    for (c = 0x80; c <= 0xFF; c++)
        ac_table[c] |= AC_VALUE;
}
