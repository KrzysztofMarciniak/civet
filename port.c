#include "port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib.h"

/* Digits only, 1..65535. Rejects "", "-1", "+80", "80x", " 80". */
s4 parse_port(const char* s, u2* out) {
        const char* p;
        u8 v;

        if (*s == '\0') return -1;

        for (p = s; *p != '\0'; p++) {
                if (*p < '0' || *p > '9') return -1;
        }

        if (p - s > 5) return -1;

        v = strtoul(s, NULL, 10);

        if (v < 1UL || v > 65535UL) return -1;

        *out = (u2)v;
        return 0;
}
