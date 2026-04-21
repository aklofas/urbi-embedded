/* SPDX-License-Identifier: BSD-3-Clause */
/* Internal arena allocator for the compiler front-end. */

#ifndef UARENA_H
#define UARENA_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque chunk in the arena's chunk list. */
typedef struct ArenaChunk ArenaChunk;

/* Pluggable backing allocator signatures.
   alloc returns NULL on failure. free must accept NULL as a no-op. */
typedef void *(*UAllocFn)(size_t nbytes, void *ud);
typedef void  (*UFreeFn)(void *ptr, void *ud);

/*
 * Arena — chunk-list bump allocator used by the compiler front-end.
 *
 * Three init modes (see uarena_init / _ex / _static below); the common
 * operations (alloc, reset, destroy) work across all modes.  All fields
 * are touched only by uarena.c — callers MUST NOT write to them.
 */
typedef struct {
    ArenaChunk *head;      /* current chunk — allocations go here */
    ArenaChunk *first;     /* list head for reset / destroy */
    size_t chunk_size;     /* new-chunk default; 0 in static-buffer mode */
    UAllocFn alloc_fn;     /* NULL in static-buffer mode */
    UFreeFn  free_fn;      /* NULL in static-buffer mode */
    void *alloc_ud;        /* userdata passed to alloc_fn / free_fn */
    bool oom;              /* sticky OOM flag */
    bool is_static;        /* true iff initialized via uarena_init_static */
} Arena;

/* Initialize using stdlib malloc / free for backing allocation.
   chunk_size == 0 selects the default (4096).
   No allocation is performed; first chunk is lazy on first uarena_alloc. */
void uarena_init(Arena *a, size_t chunk_size);

/* Initialize with a caller-supplied allocator pair.
   chunk_size == 0 selects the default (4096). */
void uarena_init_ex(Arena *a, size_t chunk_size,
                    UAllocFn alloc, UFreeFn free_fn, void *ud);

/* Initialize with a fixed caller-owned buffer.  No dynamic allocation
   is ever performed.  OOM fires when the buffer is exhausted.
   uarena_destroy is a no-op in this mode (caller owns buf). */
void uarena_init_static(Arena *a, void *buf, size_t bufsz);

/* Allocate nbytes from the arena, aligned to 16 bytes (ARENA_ALIGN,
   sufficient for long double / SIMD on all v1.0 targets).  Returned
   memory is zero-filled.  On failure (backing-allocator returns NULL,
   or static buffer is full) sets a->oom and returns NULL. */
void *uarena_alloc(Arena *a, size_t nbytes);

/* Rewind every chunk to empty, keeping backing memory for reuse.
   Clears a->oom.  O(chunks). */
void uarena_reset(Arena *a);

/* Release backing memory.  For init / init_ex: calls free_fn on each
   chunk.  For init_static: no-op. */
void uarena_destroy(Arena *a);

#ifdef __cplusplus
}
#endif

#endif
