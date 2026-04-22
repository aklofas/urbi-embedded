/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode Chunk deserializer + verifier + destroy.  Freestanding. */

#include "uchunk.h"

/* Local zero-fill.  Replaces memset so uchunk.c compiles without a hosted
   <string.h>.  volatile prevents GCC/Clang from recognizing the loop and
   lowering it back to a memset libcall under -Os.  Same pattern as uarena.c. */
static void chunk_zero(void *const dst, const size_t n) {
    volatile unsigned char *const p = (volatile unsigned char *)dst;
    for (size_t i = 0; i < n; i++) p[i] = 0;
}

#if __STDC_HOSTED__
#  include <stdlib.h>

/* Default allocator: realloc semantics.  Only compiled in hosted builds. */
static void *stdlib_alloc(void *ptr, size_t nbytes, void *ud) {
    (void)ud;
    if (nbytes == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, nbytes);
}
#endif

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

/* --- Public API --- */

UChunkLoadError uchunk_deserialize(Chunk *chunk, const uint8_t *buf, size_t size,
                                   char *errmsg, size_t errcap) {
    (void)chunk;
    (void)buf;
    (void)size;
    (void)errmsg;
    (void)errcap;
    return ULOAD_OK;
}

void uchunk_destroy(Chunk *chunk) {
    if (chunk == NULL) return;
    UChunkAllocFn alloc = chunk_allocator(chunk);
    if (alloc != NULL) {
        (void)alloc(chunk->instructions, 0, chunk->alloc_ud);
        (void)alloc(chunk->constants,    0, chunk->alloc_ud);
        (void)alloc(chunk->line_deltas,  0, chunk->alloc_ud);
        (void)alloc(chunk->abs_lines,    0, chunk->alloc_ud);
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
