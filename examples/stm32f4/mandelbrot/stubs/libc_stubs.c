/* Minimal C-runtime stubs for bare-metal arm-none-eabi builds without newlib.
 *
 * Provides the few libc symbols that the HAL + urbi library pull in:
 *   memset, memcpy, memmove, strlen — used by HAL and liburbi.a
 *   memcmp, strcmp                 — used by liburbi.a
 *   __libc_init_array             — called by startup_stm32f429xx.s
 *
 * None of these call into the OS or allocate heap. */

#include <stddef.h>
#include <stdint.h>

/* ---- memset / memcpy / memmove / strlen ---- */

void *memset(void *dst, int c, size_t n)
{
    unsigned char *p = (unsigned char *)dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (*s++) n++;
    return n;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++; pb++;
    }
    return 0;
}

int strcmp(const char *a, const char *b)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (*pa && *pa == *pb) { pa++; pb++; }
    return (int)*pa - (int)*pb;
}

/* ---- __libc_init_array ---- */
/* startup_stm32f429xx.s calls this before main() to run C++ static
 * constructors.  We have none; the stub satisfies the reference. */
void __libc_init_array(void) { }
