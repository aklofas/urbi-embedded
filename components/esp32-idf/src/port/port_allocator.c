/* SPDX-License-Identifier: BSD-3-Clause */
/* Bump + first-fit freelist allocator for urbi on the ESP32-S3.
 *
 * The arena is a static array in internal SRAM, URBI_ARENA_BYTES long
 * (CONFIG_URBI_ARENA_BYTES from the project's Kconfig).  PSRAM is left
 * to the camera's frame buffers.  The heap budget the application
 * declares to the runtime is three quarters of this size, so the
 * collector runs before the arena itself runs out.
 *
 * Every allocation carries a header; free pushes the block onto a LIFO
 * freelist, alloc walks the freelist (good-fit, then first-fit) before
 * bumping, and a large recycled block is split so a freed strand stack
 * is not wasted serving a small object.  Realloc keeps the block when it
 * is already big enough and otherwise moves the contents.
 *
 * Layout:
 *   Each allocation carries a header:
 *     { uintptr_t size; struct fl_hdr *next; size_t requested; }
 *   which is 12 bytes on the ESP32-S3 (24 on x86_64), rounded up to a
 *   multiple of 8 (FL_HDR_BYTES: 16 on the ESP32-S3, 24 on x86_64).  The
 *   returned pointer is the byte past the rounded header.  The heap base
 *   and heap_top stay 8-aligned, so every payload is 8-aligned.
 *
 * Live bytes:
 *   s_live_bytes counts the bytes callers asked for and still hold --
 *   not block sizes, not header overhead.  Alloc adds the request, free
 *   subtracts the stored request, an in-place realloc moves it by the
 *   difference, and a moving realloc is an alloc plus a free.  A refused
 *   request changes nothing.
 *
 * Lifecycle states:
 *   - in heap, unallocated (above heap_top): zero-init
 *   - in heap, allocated:  header.size = total_block, header.next ignored,
 *                          header.requested = bytes the caller asked for
 *   - in heap, freed:      header.size = total_block, header.next = freelist link
 *
 * Non-goals:
 *   - Best-fit / size-class buckets: first-fit is good enough for the
 *     <10-distinct-sizes workload urbi generates.
 *   - Coalescing adjacent freed blocks: the workload churns matched-size
 *     blocks (every strand death frees and a future spawn refills), so
 *     matched-size reuse hits before fragmentation becomes a problem.
 *   - Thread safety: only the task that owns the VM allocates -- no
 *     locks. */

#include "port_esp_idf.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Align to 8 bytes (sufficient for double / 64-bit values on the ESP32-S3 + keeps
 * subsequent fl_hdr's naturally aligned). */
#define ALIGN8(n)  (((n) + 7U) & ~7U)

/* Split a recycled block if the unused tail is at least this large
 * (in payload bytes).  Too small and we waste effort splitting; too
 * large and we waste big-block tails on small reuses.  64 B is a
 * reasonable middle for urbi's mix of UObject (56 B), UClosure (~40 B),
 * USlot (16 B), and ~32 KB strand register stacks. */
#define FL_SPLIT_MIN_TAIL_PAYLOAD  64U

static uint8_t heap[URBI_ARENA_BYTES] __attribute__((aligned(8)));
static size_t  heap_top = 0;

/* Per-allocation header.  `size` is the TOTAL block size including the
 * rounded header (lets future coalescing-by-walk land without a separate
 * length table).  `next` links freelist entries; ignored when block is
 * live.  `requested` is what the caller asked for, the unit the live
 * counter moves in. */
typedef struct fl_hdr {
    uintptr_t       size;
    struct fl_hdr  *next;
    size_t          requested;
} fl_hdr;

/* The header rounded so the payload behind it stays 8-aligned. */
#define FL_HDR_BYTES  ALIGN8(sizeof(fl_hdr))

static fl_hdr *s_freelist = NULL;

/* Diagnostic accessors — useful when OOM is unexpected (bring-up).
 * Declared in port_esp_idf.h. */
static size_t s_last_failed_request = 0;
static void  *s_last_null_ptr_arg  = NULL;
static size_t s_last_null_nbytes_arg = 0;
static size_t s_alloc_count = 0;
static size_t s_free_count  = 0;
static size_t s_freelist_hits = 0;
static size_t s_freelist_splits = 0;
static size_t s_largest_satisfied = 0;
static size_t s_live_bytes = 0;

size_t port_alloc_live_bytes(void) { return s_live_bytes; }
size_t port_alloc_heap_top(void)  { return heap_top; }
size_t port_alloc_heap_size(void) { return URBI_ARENA_BYTES; }
const void *port_alloc_heap_base(void) { return (const void *)heap; }
size_t port_alloc_last_failed_request(void) { return s_last_failed_request; }
size_t port_alloc_count(void) { return s_alloc_count; }
size_t port_alloc_free_count(void) { return s_free_count; }
size_t port_alloc_freelist_hits(void) { return s_freelist_hits; }
size_t port_alloc_freelist_splits(void) { return s_freelist_splits; }
size_t port_alloc_largest_satisfied(void) { return s_largest_satisfied; }
const void *port_alloc_last_null_ptr(void) { return s_last_null_ptr_arg; }
size_t port_alloc_last_null_nbytes(void) { return s_last_null_nbytes_arg; }

/* Pop a freelist block of total size >= need, BUT prefer blocks that
 * aren't much bigger than need (size-class isolation).
 *
 * The naive first-fit + split-on-large-reuse design destroys big blocks
 * when small allocs hit them: a freed 8 KB strand-stack is the only big
 * block on the freelist, then a 64 B sidecar alloc hits it, splits it
 * into 64 B (kept) + ~7.9 KB (tail).  Next small alloc splits the tail
 * again.  After enough small allocs the original 8 KB block is carved
 * into 100+ tiny pieces, none >= 8 KB, and the next strand-stack spawn
 * fails because no fit exists and bump is at the ceiling.
 *
 * Fix: two-pass walk.  Pass 1 looks for a block in the "good-fit" range
 * (size in [need, need * 2)).  Pass 2 falls back to first-fit if no
 * good-fit block exists.  This keeps a freed 8 KB block intact for the
 * next 8 KB request, while still letting small allocs use larger blocks
 * if no smaller freelist entry exists.  Cost: at most two walks; in the
 * common case (good-fit hit) one walk. */
static fl_hdr *
freelist_pop_fit(size_t need)
{
    fl_hdr **prev = NULL;
    fl_hdr  *cur  = NULL;

    /* Pass 1: good-fit (size in [need, need*2)). */
    prev = &s_freelist;
    cur  = s_freelist;
    while (cur != NULL) {
        if ((size_t)cur->size >= need && (size_t)cur->size < need * 2U) {
            *prev = cur->next;
            return cur;
        }
        prev = &cur->next;
        cur  = cur->next;
    }

    /* Pass 2: first-fit fallback. */
    prev = &s_freelist;
    cur  = s_freelist;
    while (cur != NULL) {
        if ((size_t)cur->size >= need) {
            *prev = cur->next;
            return cur;
        }
        prev = &cur->next;
        cur  = cur->next;
    }
    return NULL;
}

void *port_alloc(void *ptr, size_t nbytes, void *ud) {
    (void)ud;

    /* free(ptr) — nbytes == 0.  Push block (including header) onto
     * freelist LIFO. */
    if (nbytes == 0) {
        s_last_null_ptr_arg = ptr;
        s_last_null_nbytes_arg = 0;
        if (ptr == NULL) return NULL;
        fl_hdr *h = (fl_hdr *)((uint8_t *)ptr - FL_HDR_BYTES);
        h->next = s_freelist;
        s_freelist = h;
        s_live_bytes -= h->requested;
        s_free_count++;
        return NULL;
    }

    size_t payload_need = ALIGN8(nbytes);
    size_t total_need   = payload_need + FL_HDR_BYTES;

    /* Realloc — ptr != NULL, nbytes > 0.  If the existing block already
     * satisfies the new size, return it unchanged (no split — avoids
     * fragmenting the live block).  Otherwise: alloc new + memcpy
     * min(old_payload, new_payload) + free old. */
    if (ptr != NULL) {
        fl_hdr *h = (fl_hdr *)((uint8_t *)ptr - FL_HDR_BYTES);
        size_t old_payload = (size_t)h->size - FL_HDR_BYTES;
        if (old_payload >= payload_need) {
            s_live_bytes = s_live_bytes - h->requested + nbytes;
            h->requested = nbytes;
            return ptr;
        }
        void *new_ptr = port_alloc(NULL, nbytes, ud);
        if (new_ptr == NULL) {
            s_last_null_ptr_arg = ptr;
            s_last_null_nbytes_arg = nbytes;
            return NULL;
        }
        memcpy(new_ptr, ptr, old_payload);
        port_alloc(ptr, 0, ud);
        return new_ptr;
    }

    /* Fresh alloc — try freelist first. */
    fl_hdr *h = freelist_pop_fit(total_need);
    if (h != NULL) {
        /* Split the block if the unused tail is worth keeping.  Otherwise
         * give the caller the whole block (some internal slack). */
        size_t leftover = (size_t)h->size - total_need;
        if (leftover >= FL_HDR_BYTES + FL_SPLIT_MIN_TAIL_PAYLOAD) {
            fl_hdr *tail = (fl_hdr *)((uint8_t *)h + total_need);
            tail->size = leftover;
            tail->next = s_freelist;
            s_freelist = tail;
            h->size = total_need;
            s_freelist_splits++;
        }
        h->requested = nbytes;
        s_live_bytes += nbytes;
        s_freelist_hits++;
        s_alloc_count++;
        if ((size_t)h->size > s_largest_satisfied) s_largest_satisfied = (size_t)h->size;
        return (uint8_t *)h + FL_HDR_BYTES;
    }

    /* Bump from heap_top. */
    if (heap_top + total_need > URBI_ARENA_BYTES) {
        s_last_failed_request = total_need;
        s_last_null_ptr_arg = NULL;
        s_last_null_nbytes_arg = nbytes;
        return NULL;
    }
    h = (fl_hdr *)&heap[heap_top];
    h->size = total_need;
    h->next = NULL;
    h->requested = nbytes;
    s_live_bytes += nbytes;
    heap_top += total_need;
    s_alloc_count++;
    if (total_need > s_largest_satisfied) s_largest_satisfied = total_need;
    return (uint8_t *)h + FL_HDR_BYTES;
}
