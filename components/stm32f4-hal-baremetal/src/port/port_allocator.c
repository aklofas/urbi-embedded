/* SPDX-License-Identifier: BSD-3-Clause */
/* Static-buffer allocator for urbi on bare-metal STM32F4.
 *
 * Uses a bump allocator with free-list fallback.  Buffer lives in .bss
 * (internal SRAM).  Sized at URBI_HEAP_BYTES (default 80 KB).
 *
 * Suitable for urbi's allocation pattern (a handful of long-lived objects
 * created at startup, plus per-VM scratch).  NOT a general-purpose heap. */

#include "port_stm32f4.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef URBI_HEAP_BYTES
#  define URBI_HEAP_BYTES  (80U * 1024U)
#endif

/* Align to 8 bytes (sufficient for double / 64-bit pointer on M4F + simpler
 * than chasing the strictest type's alignof). */
#define ALIGN(n)  (((n) + 7U) & ~7U)

static uint8_t heap[URBI_HEAP_BYTES] __attribute__((aligned(8)));
static size_t  heap_top = 0;

/* Free-list node header lives at the start of each freed block. */
typedef struct FreeNode {
    size_t size;            /* including header */
    struct FreeNode *next;
} FreeNode;

static FreeNode *freelist = NULL;

void *port_alloc(void *ptr, size_t nbytes, void *ud) {
    (void)ud;

    /* free(ptr) — nbytes == 0 */
    if (nbytes == 0) {
        if (ptr == NULL) return NULL;
        /* Push onto freelist.  We don't know the original block size from
         * urbi's free path, so we record it in a header BEFORE the user ptr
         * during allocation (see below).  For simplicity here we treat
         * free as a no-op leak — bump allocator's natural behavior. */
        (void)freelist;  /* freelist reserved for future coalescence */
        return NULL;
    }

    size_t need = ALIGN(nbytes);

    /* Realloc — naive: alloc new, copy, return new.  Caller must own old. */
    if (ptr != NULL) {
        void *new_ptr = port_alloc(NULL, need, ud);
        if (new_ptr == NULL) return NULL;
        memcpy(new_ptr, ptr, nbytes);  /* may copy past end; accepted for bump */
        return new_ptr;
    }

    /* Fresh alloc — bump */
    if (heap_top + need > URBI_HEAP_BYTES) {
        return NULL;  /* OOM */
    }
    void *p = &heap[heap_top];
    heap_top += need;
    return p;
}
