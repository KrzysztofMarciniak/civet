#include "debug.h"

#ifdef DEBUG
#include <stdio.h>
#include <stdarg.h>

void debug_log(const char* prefix, const char* fmt, ...) {
	va_list ap;

	fprintf(stderr, "DEBUG %s: ", prefix);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	fflush(stderr);
}

#endif
