/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode Chunk deserializer + verifier + destroy.  Freestanding. */

#include "uchunk.h"
#include "uchunk_internal.h"

#include <stdarg.h>               /* va_list / va_start / va_end — freestanding-ok */

/* Local zero-fill.  Replaces memset so uchunk.c compiles without a hosted
   <string.h>.  volatile prevents GCC/Clang from recognizing the loop and
   lowering it back to a memset libcall under -Os.  Same pattern as uarena.c. */
static void chunk_zero(void *const dst, const size_t n) {
    volatile unsigned char *const p = (volatile unsigned char *)dst;
    for (size_t i = 0; i < n; i++) p[i] = 0;
}

/* Local byte-compare.  Replaces memcmp so uchunk.c compiles without
   <string.h> under -ffreestanding. */
static int chunk_memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

/* Canonical canary bytes — LANG-CONVENTIONS §4 / spec §5.1. */
static const uint8_t kCanary[6] = { 0x19, 0x93, '\r', '\n', 0x1A, '\n' };

#if __STDC_HOSTED__
#  include <stdio.h>
#  include <stdlib.h>

/* Safe snprintf-style helper. No-op when errmsg==NULL or errcap==0. */
static void set_errmsg(char *errmsg, size_t errcap, const char *fmt, ...) {
    if (errmsg == NULL || errcap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(errmsg, errcap, fmt, ap);
    va_end(ap);
}

/* Default allocator: realloc semantics.  Only compiled in hosted builds. */
static void *stdlib_alloc(void *ptr, size_t nbytes, void *ud) {
    (void)ud;
    if (nbytes == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, nbytes);
}
#else  /* freestanding */

/* No-op: freestanding builds suppress diagnostic messages entirely. */
static void set_errmsg(char *errmsg, size_t errcap, const char *fmt, ...) {
    (void)errmsg;
    (void)errcap;
    (void)fmt;
}
#endif  /* __STDC_HOSTED__ */

/* Resolve the effective allocator for a chunk. */
static UChunkAllocFn chunk_allocator(const Chunk *c) {
#if __STDC_HOSTED__
    return c->alloc_fn != NULL ? c->alloc_fn : stdlib_alloc;
#else
    /* Freestanding: caller MUST supply alloc_fn.  NULL here is a programming
       error and chunk_grow will propagate it as OOM. */
    return c->alloc_fn;
#endif
}

/* --- Varint decode helpers --- */

/* LEB128-style unsigned varint decoder.  7 payload bits per byte; top bit
   is the continuation flag.  Max 10 bytes for a uint64. */
UChunkLoadError varint_decode_u(const uint8_t *buf, size_t size,
                                uint64_t *v, size_t *consumed) {
    uint64_t result = 0;
    size_t i = 0;
    unsigned shift = 0;
    for (i = 0; i < size; i++) {
        uint8_t b = buf[i];
        result |= (uint64_t)(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0u) {
            *v = result;
            *consumed = i + 1;
            return ULOAD_OK;
        }
        shift += 7;
        if (shift > 63u) {
            return ULOAD_CORRUPT_VARINT;
        }
    }
    return ULOAD_TRUNCATED;
}

/* Zigzag-signed varint decoder. */
UChunkLoadError varint_decode_zz(const uint8_t *buf, size_t size,
                                 int64_t *v, size_t *consumed) {
    uint64_t u = 0;
    UChunkLoadError rc = varint_decode_u(buf, size, &u, consumed);
    if (rc != ULOAD_OK) return rc;
    /* zigzag decode: (u >> 1) ^ -(u & 1) */
    *v = (int64_t)((u >> 1) ^ (uint64_t)(-(int64_t)(u & 1u)));
    return ULOAD_OK;
}

/* --- Public API --- */

UChunkLoadError uchunk_deserialize(Chunk *chunk, const uint8_t *buf, size_t size,
                                   char *errmsg, size_t errcap) {
    if (chunk == NULL || buf == NULL) {
        set_errmsg(errmsg, errcap, "null chunk or buffer");
        return ULOAD_TRUNCATED;
    }

    /* --- 24-byte header --- */
    if (size < 24u) {
        set_errmsg(errmsg, errcap,
                   "buffer truncated at header (got %zu bytes, need 24)", size);
        return ULOAD_TRUNCATED;
    }

    /* magic "URBI" at bytes 0-3 */
    if (buf[0] != 'U' || buf[1] != 'R' || buf[2] != 'B' || buf[3] != 'I') {
        set_errmsg(errmsg, errcap, "bad magic (expected \"URBI\")");
        return ULOAD_BAD_MAGIC;
    }

    /* version byte: 0x10 = v1.0 (16*major + minor) */
    if (buf[4] != 0x10u) {
        set_errmsg(errmsg, errcap,
                   "unsupported version byte 0x%02x", (unsigned)buf[4]);
        return ULOAD_UNSUPPORTED_VERSION;
    }

    /* buf[5] = flags; no flag bits defined at v1.0, ignored for forward-compat */

    /* canary bytes at offsets 6-11 */
    if (chunk_memcmp(buf + 6, kCanary, sizeof kCanary) != 0) {
        set_errmsg(errmsg, errcap,
                   "corrupt canary bytes (possible FTP/Windows paste translation)");
        return ULOAD_BAD_MAGIC;
    }

    /* format descriptor fields, one at a time for specific diagnostics */
    if (buf[12] != (uint8_t)URBI_INT_WIDTH) {
        set_errmsg(errmsg, errcap,
                   "flavor mismatch: int_width expected %u, got %u",
                   (unsigned)URBI_INT_WIDTH, (unsigned)buf[12]);
        return ULOAD_FLAVOR_MISMATCH;
    }
    if (buf[13] != (uint8_t)URBI_FLOAT_TYPE) {
        set_errmsg(errmsg, errcap,
                   "flavor mismatch: float_type expected %u, got %u",
                   (unsigned)URBI_FLOAT_TYPE, (unsigned)buf[13]);
        return ULOAD_FLAVOR_MISMATCH;
    }
    if (buf[14] != (uint8_t)URBI_INSTR_WIDTH) {
        set_errmsg(errmsg, errcap,
                   "flavor mismatch: instr_width expected %u, got %u",
                   (unsigned)URBI_INSTR_WIDTH, (unsigned)buf[14]);
        return ULOAD_FLAVOR_MISMATCH;
    }
    if (buf[15] != (uint8_t)URBI_ENDIANNESS) {
        set_errmsg(errmsg, errcap,
                   "flavor mismatch: endianness expected %u, got %u",
                   (unsigned)URBI_ENDIANNESS, (unsigned)buf[15]);
        return ULOAD_FLAVOR_MISMATCH;
    }
    /* buf[16..23] reserved — not validated (forward-compat) */

    /* --- body decode stub — Task 5 fills in --- */
    (void)errcap;
    (void)errmsg;
    return ULOAD_OK;
}

void uchunk_destroy(Chunk *chunk) {
    if (chunk == NULL) return;
    UChunkAllocFn alloc = chunk_allocator(chunk);
    if (alloc != NULL) {
        if (chunk->instructions != NULL) (void)alloc(chunk->instructions, 0, chunk->alloc_ud);
        if (chunk->constants    != NULL) (void)alloc(chunk->constants,    0, chunk->alloc_ud);
        if (chunk->line_deltas  != NULL) (void)alloc(chunk->line_deltas,  0, chunk->alloc_ud);
        if (chunk->abs_lines    != NULL) (void)alloc(chunk->abs_lines,    0, chunk->alloc_ud);
    }
    /* Zero the entire struct — preserves no fields (source_name, alloc_fn,
       alloc_ud are all reset; caller must re-init before re-use). */
    chunk_zero(chunk, sizeof(*chunk));
}

const char *uchunk_load_error_name(UChunkLoadError code) {
    switch (code) {
    case ULOAD_OK:                  return "ULOAD_OK";
    case ULOAD_BAD_MAGIC:           return "ULOAD_BAD_MAGIC";
    case ULOAD_UNSUPPORTED_VERSION: return "ULOAD_UNSUPPORTED_VERSION";
    case ULOAD_FLAVOR_MISMATCH:     return "ULOAD_FLAVOR_MISMATCH";
    case ULOAD_TRUNCATED:           return "ULOAD_TRUNCATED";
    case ULOAD_CORRUPT_VARINT:      return "ULOAD_CORRUPT_VARINT";
    case ULOAD_CORRUPT_TAG:         return "ULOAD_CORRUPT_TAG";
    case ULOAD_CORRUPT:             return "ULOAD_CORRUPT";
    case ULOAD_OOM:                 return "ULOAD_OOM";
    }
    return "ULOAD_UNKNOWN";
}
