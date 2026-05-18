/* Minimal stdio.h shim for bare-metal arm-none-eabi builds without newlib.
 * Implements vsnprintf (used by port_diag.c) covering %s, %d, %u, %x, %c, %%
 * and common width/precision modifiers.  Not a complete POSIX vsnprintf. */
#ifndef STUBS_STDIO_H
#define STUBS_STDIO_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* Forward declaration. */
static int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
static int snprintf(char *buf, size_t cap, const char *fmt, ...);

/* ---- helpers ---- */
size_t strlen(const char *s);
static inline int stubs_strlen(const char *s) { return (int)strlen(s); }

static inline int stubs_put(char *buf, size_t cap, int *pos, char c) {
    if ((size_t)(*pos) < cap - 1U) { buf[(*pos)] = c; }
    (*pos)++;
    return 0;
}

static void stubs_putu(char *buf, size_t cap, int *pos,
                        unsigned long v, int base, int width, int pad0) {
    char tmp[22];
    int  len = 0;
    if (v == 0) { tmp[len++] = '0'; }
    else {
        while (v) {
            unsigned r = (unsigned)(v % (unsigned long)base);
            tmp[len++] = (char)(r < 10u ? '0' + r : 'a' + r - 10u);
            v /= (unsigned long)base;
        }
    }
    while (width > len) { stubs_put(buf, cap, pos, pad0 ? '0' : ' '); width--; }
    while (len > 0) { stubs_put(buf, cap, pos, tmp[--len]); }
}

static int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    int pos = 0;
    if (!buf || cap == 0) return 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') { stubs_put(buf, cap, &pos, *fmt); continue; }
        fmt++;
        int pad0  = (*fmt == '0');
        if (pad0) fmt++;
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        /* precision ignored */
        if (*fmt == '.') { fmt++; while (*fmt >= '0' && *fmt <= '9') fmt++; }
        /* length modifier */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') fmt++; }
        if (*fmt == 'z') { is_long = 1; fmt++; }
        switch (*fmt) {
            case 'd': case 'i': {
                long v = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
                if (v < 0) { stubs_put(buf, cap, &pos, '-'); v = -v; if (width) width--; }
                stubs_putu(buf, cap, &pos, (unsigned long)v, 10, width, pad0);
                break;
            }
            case 'u': {
                unsigned long v = is_long ? va_arg(ap, unsigned long)
                                          : (unsigned long)va_arg(ap, unsigned);
                stubs_putu(buf, cap, &pos, v, 10, width, pad0);
                break;
            }
            case 'x': case 'X': {
                unsigned long v = is_long ? va_arg(ap, unsigned long)
                                          : (unsigned long)va_arg(ap, unsigned);
                stubs_putu(buf, cap, &pos, v, 16, width, pad0);
                break;
            }
            case 'p': {
                unsigned long v = (unsigned long)(uintptr_t)va_arg(ap, void *);
                stubs_put(buf, cap, &pos, '0');
                stubs_put(buf, cap, &pos, 'x');
                stubs_putu(buf, cap, &pos, v, 16, 0, 0);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                int len = stubs_strlen(s);
                while (width > len) { stubs_put(buf, cap, &pos, ' '); width--; }
                while (*s) { stubs_put(buf, cap, &pos, *s++); }
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                stubs_put(buf, cap, &pos, c);
                break;
            }
            case '%': stubs_put(buf, cap, &pos, '%'); break;
            default:  stubs_put(buf, cap, &pos, '%');
                      stubs_put(buf, cap, &pos, *fmt);
                      break;
        }
    }
    if (cap > 0) buf[pos < (int)(cap-1) ? pos : (int)(cap-1)] = '\0';
    return pos;
}

static int snprintf(char *buf, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return n;
}

#endif /* STUBS_STDIO_H */
