/* lib.h - fixed-width types for C89 (which has no <stdint.h>).
 *
 * The digit is the size in BYTES:
 *
 *     u1 s1   8 bit      u2 s2  16 bit
 *     u4 s4  32 bit      u8 s8  64 bit (only if the platform has one)
 *
 * Types are picked from <limits.h> at compile time. If the platform can't
 * provide an exact-width type, compilation stops with #error instead of
 * silently getting the wrong size.
 *
 * Use these for data whose size matters: raw bytes, protocol fields,
 * counters, limits. Keep plain `char` for text, and the libc/POSIX types
 * (size_t, ssize_t, int fds, int return codes) wherever the system
 * interfaces demand them.
 */
#ifndef LIB_H
#define LIB_H

#include <limits.h>

/* ---- 8 bit ---- */
#if CHAR_BIT != 8
#error "lib.h: CHAR_BIT must be 8"
#endif
typedef unsigned char u1;
typedef signed char s1;

/* ---- 16 bit ---- */
#if USHRT_MAX == 0xFFFF && SHRT_MAX == 0x7FFF
typedef unsigned short u2;
typedef short s2;
#else
#error "lib.h: no 16-bit integer type"
#endif

/* ---- 32 bit ---- */
#if UINT_MAX == 0xFFFFFFFF && INT_MAX == 0x7FFFFFFF
typedef unsigned int u4;
typedef int s4;
#elif ULONG_MAX == 0xFFFFFFFF && LONG_MAX == 0x7FFFFFFF
typedef unsigned long u4;
typedef long s4;
#else
#error "lib.h: no 32-bit integer type"
#endif

/* ---- 64 bit ---- */
#if ULONG_MAX > 0xFFFFFFFF
#if ULONG_MAX == 0xFFFFFFFFFFFFFFFF && LONG_MAX == 0x7FFFFFFFFFFFFFFF
typedef unsigned long u8;
typedef long s8;
#define LIB_HAVE_64 1
#endif
#endif

/* ---- compile-time size checks (C89 static assert) ----
 * A false condition makes a negative array size, which is a compile error. */
#define LIB_STATIC_ASSERT(name, cond) \
        typedef char lib_assert_##name[(cond) ? 1 : -1]

LIB_STATIC_ASSERT(u1, sizeof(u1) == 1 && sizeof(s1) == 1);
LIB_STATIC_ASSERT(u2, sizeof(u2) == 2 && sizeof(s2) == 2);
LIB_STATIC_ASSERT(u4, sizeof(u4) == 4 && sizeof(s4) == 4);
#ifdef LIB_HAVE_64
LIB_STATIC_ASSERT(u8, sizeof(u8) == 8 && sizeof(s8) == 8);
#endif

/* ---- limits ---- */
#define U1_MAX 0xFFU
#define U2_MAX 0xFFFFU
#define U4_MAX 0xFFFFFFFFUL
#define S1_MAX 0x7F
#define S2_MAX 0x7FFF
#define S4_MAX 0x7FFFFFFFL

#endif
