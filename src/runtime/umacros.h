/* SPDX-License-Identifier: BSD-3-Clause */
/* Project-wide internal macros shared across the runtime.
 * Everything under src/ is internal by definition (the public C API lives in
 * include/urbi/), so embedders never include this header.
 *
 * Note: URBI_ASSERT_NOT_ISR is intentionally NOT defined here — it lives in
 * include/urbi/urbi.h because it is part of the embedder-facing assertion
 * surface (host bridges can use it for ISR-unsafe-entry catches in debug
 * builds).  Per Wave-1 audit GC-012 the location is documented but not
 * relocated; the deeper hygiene of "public macro touching internal field"
 * carries forward to wave-3-naming. */

#ifndef UMACROS_H
#define UMACROS_H

#if __STDC_HOSTED__
#  include <assert.h>
#  define URBI_INTERNAL_ASSERT(cond) assert(cond)
#else
#  define URBI_INTERNAL_ASSERT(cond) ((void)0)
#endif

/* urbi_zero — freestanding-discipline byte-zero with `volatile` to defeat
 * dead-store elimination on caller-provided memory.  Single source of truth
 * for the per-subsystem `*_zero` helpers retired during v0.5.4-decompose
 * (CHSTR-021 + REALM-020 + MOD-021 + WATCH-027).
 *
 * Contract: writes exactly `n` bytes of `0` starting at `p`.  Caller-provided
 * memory must be at least `n` bytes; this helper does NOT bound-check.
 *
 * Why volatile: in freestanding builds we cannot assume `memset` is
 * available (no libc) AND we must guarantee zeroing of caller buffers that
 * the compiler might otherwise optimize away (e.g., zeroing a UStrand
 * scratch frame on the C stack just before a call to `setjmp`-style unwind).
 */
#include <stddef.h>

static inline void urbi_zero(void *const dst, const size_t n) {
    volatile unsigned char *const p = (volatile unsigned char *)dst;
    for (size_t i = 0; i < n; ++i) {
        p[i] = 0;
    }
}

/* urbi_strlen — freestanding strlen (no <string.h>).
 * Consolidates per-file strlen helpers (rg_strlen in urealm_globals.c
 * at v0.5.4; emit_strlen in uemit_internal.h at v0.5.5) into a single
 * shared helper.  REALM-022. */
static inline size_t urbi_strlen(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

/* urbi_strncpy_truncating — freestanding bounded string copy.
 * Copies at most (cap - 1) bytes from src into dst and always NUL-terminates
 * dst when cap > 0.  No-op when cap == 0.  Consolidates per-file strncpy
 * helpers (chunk_strncpy in uchunk.c) into a single shared helper.
 * CHSTR-020. */
static inline void urbi_strncpy_truncating(char *dst, size_t cap, const char *src) {
    size_t n = 0;
    if (cap == 0) return;
    while (n + 1U < cap && src[n] != '\0') {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

/* urbi_memcpy — plain byte-copy loop, no <string.h> dependency.
 *
 * Unlike urbi_zero, no volatile qualifier is needed: dead-store elimination
 * is not a risk for copies (the compiler can see the source is live).
 *
 * Contract: copies exactly `n` bytes from `src` to `dst`.  Regions must not
 * overlap (same contract as standard memcpy).  Caller must ensure both
 * pointers are valid for `n` bytes. */
static inline void urbi_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

/* urbi_memeq: byte-equality for two memory regions of equal length.
 *
 * Equivalent to `memcmp(a, b, n) == 0` but with no <string.h> dependency.
 * Useful in -ffreestanding builds where libc is unavailable.
 *
 * Returns 1 if every byte of [a, a+n) equals the corresponding byte of
 * [b, b+n); 0 otherwise. Behavior is defined for n == 0 (returns 1).
 *
 * Pre-existing local equivalents (`lex_memeq`, `module_memcmp`) are
 * retired in favor of this helper at v0.5.5 (Wave 3). */
static inline int urbi_memeq(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return 0;
    }
    return 1;
}

#endif /* UMACROS_H */
