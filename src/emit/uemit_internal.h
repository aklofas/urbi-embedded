/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_internal.h — private inter-TU helpers for the emit subsystem.
 *
 * Consumed only by src/emit/ TUs.  Public emit API is in src/emit/uemit.h.
 * Do NOT include from outside src/emit/. */

#ifndef UEMIT_INTERNAL_H
#define UEMIT_INTERNAL_H

#include "uemit.h"
#include "util/umacros.h"   /* urbi_strlen */

#include <stddef.h>
#include <stdint.h>

/* Local byte-copy.  Replaces memcpy so the serializer compiles without
   a hosted <string.h>.  Same pattern as module_memcpy in chunk/uchunk_io.c. */
static inline void emit_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *pd = (unsigned char *)dst;
    const unsigned char *ps = (const unsigned char *)src;
    size_t i;
    for (i = 0U; i < n; i++) pd[i] = ps[i];
}

/* --- Module allocator helper --- */

#if __STDC_HOSTED__
#  include <stdlib.h>

static inline void *emit_stdlib_alloc(void *ptr, size_t nbytes, void *ud) {
    (void)ud;
    if (nbytes == 0U) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

#endif  /* __STDC_HOSTED__ */

/* Return the allocator to use for root UProto c.  In freestanding builds
   the stdlib fallback is absent and the caller must have supplied
   alloc_fn. */
static inline UChunkAllocFn emit_alloc_for(const UProto *c) {
#if __STDC_HOSTED__
    return c->alloc_fn != NULL ? c->alloc_fn : emit_stdlib_alloc;
#else
    return c->alloc_fn;   /* freestanding: caller must supply */
#endif
}

/* OP_JMP's Bx is a signed offset biased by this much, so Bx 0x0000 means
   "jump back 0x8000" and 0x8000 means "offset 0". */
#define UEMIT_JMP_BIAS                32768U

#endif /* UEMIT_INTERNAL_H */
