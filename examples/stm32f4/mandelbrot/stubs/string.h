/* Minimal string.h shim for bare-metal arm-none-eabi builds without newlib.
 * Declares the C library functions that are defined in stubs/libc_stubs.c.
 * Standard declarations are sufficient — the linker resolves them to our
 * own implementations, which do not need a hosted C runtime. */
#ifndef STUBS_STRING_H
#define STUBS_STRING_H

#include <stddef.h>

void  *memcpy (void *dst, const void *src, size_t n);
void  *memset (void *dst, int c, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
size_t strlen (const char *s);
int    memcmp (const void *a, const void *b, size_t n);

#endif /* STUBS_STRING_H */
