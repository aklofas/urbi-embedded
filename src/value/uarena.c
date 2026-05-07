/* SPDX-License-Identifier: BSD-3-Clause */
/* UArena allocator implementation. */

#include "value/uarena.h"
#include <stdint.h>

#if __STDC_HOSTED__
#include <stdlib.h>
#endif /* __STDC_HOSTED__ */

/* Per-chunk layout: header immediately followed by the slab.
   Payload begins at chunk + 1 aligned to ARENA_ALIGN. */
struct UArenaChunk {
    UArenaChunk *next;
    size_t capacity;   /* payload bytes */
    size_t used;       /* consumed payload bytes */
    /* payload follows as a flexible-array-like region (see chunk_payload). */
};

#define ARENA_DEFAULT_CHUNK_SIZE 4096
/* Alignment quantum.  alignof(max_align_t) would be ideal but both are C11
   and the project is locked to -std=c99 per v1.0 impl-design.md §6 Q1.
   Hardcode 16: it covers long double, void*, and common SIMD types on all
   v1.0 targets (x86_64, ARM Cortex-M7, RISC-V 32-bit, Xtensa LX7).
   Over-aligns by 8 bytes per-allocation on 32-bit targets — negligible at
   our allocation rate (handful of AstNodes per statement). */
#define ARENA_ALIGN 16

/* --- Chunk payload address and size helpers. --- */

static unsigned char *chunk_payload(UArenaChunk *c) {
    /* Skip the header, aligning the payload start to ARENA_ALIGN. */
    uintptr_t base = (uintptr_t)(c + 1);
    uintptr_t misalign = base % ARENA_ALIGN;
    if (misalign) base += ARENA_ALIGN - misalign;
    return (unsigned char *)base;
}

/* --- Local zero-fill.  Replaces memset so the arena compiles without a
       hosted <string.h>.  volatile prevents GCC/Clang from recognizing the
       loop and lowering it back to a memset libcall under -Os.  Called with
       small (typically 16-64 byte) aligned ranges so the byte loop is not a
       meaningful hot path. --- */

static void arena_zero(void *const dst, const size_t n) {
    volatile unsigned char *const p = (volatile unsigned char *)dst;
    for (size_t i = 0; i < n; i++) p[i] = 0;
}

/* --- stdlib default allocator pair (hosted only). --- */

#if __STDC_HOSTED__
static void *stdlib_alloc(size_t n, void *ud) {
    (void)ud;
    return malloc(n);
}

static void stdlib_free(void *p, void *ud) {
    (void)ud;
    free(p);
}
#endif /* __STDC_HOSTED__ */

/* --- Common init. --- */

static void init_common(UArena *a, size_t chunk_size,
                        UAllocFn alloc, UFreeFn free_fn, void *ud) {
    a->head = NULL;
    a->first = NULL;
    a->chunk_size = chunk_size ? chunk_size : ARENA_DEFAULT_CHUNK_SIZE;
    a->alloc_fn = alloc;
    a->free_fn = free_fn;
    a->alloc_ud = ud;
    a->oom = false;
    a->is_static = false;
}

#if __STDC_HOSTED__
void uarena_init(UArena *a, size_t chunk_size) {
    init_common(a, chunk_size, stdlib_alloc, stdlib_free, NULL);
}
#endif /* __STDC_HOSTED__ */

void uarena_init_ex(UArena *a, size_t chunk_size,
                    UAllocFn alloc, UFreeFn free_fn, void *ud) {
    init_common(a, chunk_size, alloc, free_fn, ud);
}

void uarena_init_static(UArena *a, void *buf, size_t bufsz) {
    a->head = NULL;
    a->first = NULL;
    a->chunk_size = 0;
    a->alloc_fn = NULL;
    a->free_fn = NULL;
    a->alloc_ud = NULL;
    a->oom = false;
    a->is_static = true;

    /* Carve the caller-provided buffer into one header + payload chunk.
       If the buffer is too small to hold the header, OOM fires on the
       first uarena_alloc (head remains NULL). */
    if (bufsz >= sizeof(UArenaChunk) + ARENA_ALIGN) {
        UArenaChunk *c = (UArenaChunk *)buf;
        c->next = NULL;
        c->used = 0;
        /* Compute payload capacity after header-and-alignment skip. */
        unsigned char *payload = chunk_payload(c);
        uintptr_t payload_off = (uintptr_t)payload - (uintptr_t)buf;
        c->capacity = bufsz - payload_off;
        a->first = c;
        a->head = c;
    }
}

/* --- alloc. --- */

static UArenaChunk *new_chunk(UArena *a, size_t min_payload) {
    size_t want = a->chunk_size;
    if (min_payload > want) want = min_payload;
    size_t raw = sizeof(UArenaChunk) + ARENA_ALIGN + want;
    void *mem = a->alloc_fn(raw, a->alloc_ud);
    if (!mem) return NULL;
    UArenaChunk *c = mem;
    c->next = NULL;
    c->used = 0;
    unsigned char *payload = chunk_payload(c);
    uintptr_t payload_off = (uintptr_t)payload - (uintptr_t)mem;
    c->capacity = raw - payload_off;
    return c;
}

void *uarena_alloc(UArena *a, size_t nbytes) {
    if (a->oom) return NULL;

    /* Round request up to alignment. */
    size_t need = nbytes;
    if (need % ARENA_ALIGN) need += ARENA_ALIGN - (need % ARENA_ALIGN);

    /* Grab or create a chunk with enough room. */
    UArenaChunk *c = a->head;
    if (!c || c->used + need > c->capacity) {
        if (a->is_static) {
            /* No dynamic allocation in static mode. */
            a->oom = true;
            return NULL;
        }
        c = new_chunk(a, need);
        if (!c) {
            a->oom = true;
            return NULL;
        }
        /* Link at tail to preserve "first" for reset ordering. */
        if (a->head) {
            a->head->next = c;
        } else {
            a->first = c;
        }
        a->head = c;
    }

    unsigned char *p = chunk_payload(c) + c->used;
    c->used += need;
    arena_zero(p, need);
    return p;
}

/* --- reset. --- */

void uarena_reset(UArena *a) {
    for (UArenaChunk *c = a->first; c; c = c->next) c->used = 0;
    a->head = a->first;
    a->oom = false;
}

/* --- destroy. --- */

void uarena_destroy(UArena *a) {
    if (a->is_static) {
        /* Caller owns the buffer. */
        a->head = a->first = NULL;
        return;
    }
    UArenaChunk *c = a->first;
    while (c) {
        UArenaChunk *next = c->next;
        a->free_fn(c, a->alloc_ud);
        c = next;
    }
    a->head = a->first = NULL;
}
