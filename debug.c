#include "debug.h"

#include <stdarg.h>
#include <stdio.h>

#ifdef DEBUG
void debug_log(const char* prefix, const char* fmt, ...) {
        va_list ap;

        fprintf(stderr, "DEBUG %s: ", prefix);
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fprintf(stderr, "\n");
        fflush(stderr);
}
#else
void debug_log(const char* prefix, const char* fmt, ...) {
        (void)prefix;
        (void)fmt;
}
#endif
