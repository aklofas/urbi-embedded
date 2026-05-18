/* Minimal C-runtime stubs for bare-metal arm-none-eabi builds without newlib.
 *
 * Provides the few libc symbols that the HAL + urbi library pull in:
 *   memset, memcpy, memmove, strlen — used by HAL and liburbi.a
 *   __libc_init_array             — called by startup_stm32f429xx.s
 *   urbi_event_emit_from_isr      — alias to urbi_inject_event (used by
 *                                   port_button.c when not in URBI_PORT_TEST
 *                                   mode; see port_button.c line 18)
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

/* ---- __libc_init_array ---- */
/* startup_stm32f429xx.s calls this before main() to run C++ static
 * constructors.  We have none; the stub satisfies the reference. */
void __libc_init_array(void) { }

/* ---- urbi_event_emit_from_isr ---- */
/* port_button.c declares this as extern and calls it from the EXTI ISR.
 * It is not a public urbi API symbol — it is an internal ring helper.
 * Forward to urbi_inject_event (the public ISR-safe primitive) with a
 * NULL/0 payload, which is what port_button.c intends. */
int urbi_inject_event(void *vm, uint32_t event_id,
                      const void *payload, size_t len);

int urbi_event_emit_from_isr(void *vm, uint32_t event_id, void *payload)
{
    (void)payload;
    return urbi_inject_event(vm, event_id, (const void *)0, 0);
}
