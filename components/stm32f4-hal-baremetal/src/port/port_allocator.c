/* SPDX-License-Identifier: BSD-3-Clause */
/* Bump + first-fit freelist allocator for urbi on bare-metal STM32F4.
 *
 * Upgrade history:
 *   v0.8.2 initial: leak-only bump.  Each watcher body strand spawn
 *     leaked its 32 KB register stack on death; the 1 MB SDRAM heap
 *     filled in ~32 spawns and every subsequent at-handler died with
 *     "out of memory (stack alloc)" (same failure mode as v0.7.2
 *     eye_demo before T20 eager-reap landed).
 *   v0.8.2 freelist (this file): per-allocation 8-byte (M4) header
 *     [size, next], free pushes block onto LIFO freelist, alloc walks
 *     freelist first-fit before bumping, large-block reuses are split
 *     so a 32 KB strand-stack block freed by one strand death is not
 *     wasted serving a 256 B UClosure alloc.  Realloc preserves contents
 *     via min(old_payload, new_size) memcpy.
 *
 * Layout:
 *   Each allocation carries an 8-byte (M4) / 16-byte (x86_64) header:
 *     { uintptr_t size; struct fl_hdr *next; }
 *   Returned pointer is the byte past the header.  Header is 8-aligned
 *   (heap base + heap_top stay 8-aligned via ALIGN8), so the returned
 *   payload pointer is also 8-aligned.
 *
 * Lifecycle states:
 *   - in heap, unallocated (above heap_top): zero-init
 *   - in heap, allocated:  header.size = total_block, header.next ignored
 *   - in heap, freed:      header.size = total_block, header.next = freelist link
 *
 * Two heap-placement modes (unchanged from initial design):
 *   1. Internal SRAM (default): static array in .bss, sized URBI_HEAP_BYTES
 *      (default 80 KB).  Fast but limited by SRAM (F429 = 192 KB total).
 *   2. External SDRAM (define URBI_HEAP_EXTERNAL_ADDR at build time):
 *      heap points to a fixed SDRAM region.  Caller must initialize the
 *      SDRAM controller (e.g., BSP_SDRAM_Init) before the first port_alloc
 *      call.  Slower (~150 ns access vs ~50 ns SRAM through cache) but
 *      lifts the size limit — F429I-DISC1 has 8 MB external SDRAM.
 *
 * Non-goals (deferred):
 *   - Best-fit / size-class buckets: first-fit is good enough for the
 *     <10-distinct-sizes workload urbi generates; bucketed allocators
 *     can land if profiling shows fragmentation hurts.
 *   - Coalescing adjacent freed blocks: the workload churns matched-size
 *     blocks (every strand death frees and a future spawn refills);
 *     without coalescing we miss merging neighboring frees but in
 *     practice the matched-size reuse hits before fragmentation becomes
 *     a problem.  Add coalescing later if profiling shows accumulating
 *     small fragments.
 *   - Thread safety: single-threaded VM (URBI_SCHED_COOPERATIVE) — no
 *     locks.  Multi-VM-per-process (v1.x) will need per-VM heaps. */

#include "port_stm32f4.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef URBI_HEAP_BYTES
#  define URBI_HEAP_BYTES  (80U * 1024U)
#endif

/* Align to 8 bytes (sufficient for double / 64-bit pointer on M4F + keeps
 * subsequent fl_hdr's naturally aligned). */
#define ALIGN8(n)  (((n) + 7U) & ~7U)

/* Split a recycled block if the unused tail is at least this large
 * (in payload bytes).  Too small and we waste effort splitting; too
 * large and we waste big-block tails on small reuses.  64 B is a
 * reasonable middle for urbi's mix of UObject (56 B), UClosure (~40 B),
 * USlot (16 B), and ~32 KB strand register stacks. */
#define FL_SPLIT_MIN_TAIL_PAYLOAD  64U

#ifdef URBI_HEAP_EXTERNAL_ADDR
static uint8_t *const heap = (uint8_t *)URBI_HEAP_EXTERNAL_ADDR;
#else
static uint8_t heap[URBI_HEAP_BYTES] __attribute__((aligned(8)));
#endif
static size_t  heap_top = 0;

/* Per-allocation header.  `size` is the TOTAL block size including this
 * header (lets future coalescing-by-walk land without a separate length
 * table).  `next` links freelist entries; ignored when block is live. */
typedef struct fl_hdr {
    uintptr_t       size;
    struct fl_hdr  *next;
} fl_hdr;

static fl_hdr *s_freelist = NULL;

/* Diagnostic accessors — useful when OOM is unexpected (bring-up).
 * Externs declared in callers (e.g. examples/stm32f4/mandelbrot/main.c). */
static size_t s_last_failed_request = 0;
static void  *s_last_null_ptr_arg  = NULL;
static size_t s_last_null_nbytes_arg = 0;
static size_t s_alloc_count = 0;
static size_t s_free_count  = 0;
static size_t s_freelist_hits = 0;
static size_t s_freelist_splits = 0;
static size_t s_largest_satisfied = 0;
size_t port_alloc_heap_top(void)  { return heap_top; }
size_t port_alloc_heap_size(void) { return URBI_HEAP_BYTES; }
const void *port_alloc_heap_base(void) { return (const void *)heap; }
size_t port_alloc_last_failed_request(void) { return s_last_failed_request; }
size_t port_alloc_count(void) { return s_alloc_count; }
size_t port_alloc_free_count(void) { return s_free_count; }
size_t port_alloc_freelist_hits(void) { return s_freelist_hits; }
size_t port_alloc_freelist_splits(void) { return s_freelist_splits; }
size_t port_alloc_largest_satisfied(void) { return s_largest_satisfied; }
const void *port_alloc_last_null_ptr(void) { return s_last_null_ptr_arg; }
size_t port_alloc_last_null_nbytes(void) { return s_last_null_nbytes_arg; }

/* Pop a freelist block of total size >= need.  First-fit walk; on hit
 * unlinks and returns. */
static fl_hdr *
freelist_pop_fit(size_t need)
{
    fl_hdr **prev = &s_freelist;
    fl_hdr  *cur  = s_freelist;
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
        fl_hdr *h = (fl_hdr *)((uint8_t *)ptr - sizeof(fl_hdr));
        h->next = s_freelist;
        s_freelist = h;
        s_free_count++;
        return NULL;
    }

    size_t payload_need = ALIGN8(nbytes);
    size_t total_need   = payload_need + sizeof(fl_hdr);

    /* Realloc — ptr != NULL, nbytes > 0.  If the existing block already
     * satisfies the new size, return it unchanged (no split — avoids
     * fragmenting the live block).  Otherwise: alloc new + memcpy
     * min(old_payload, new_payload) + free old. */
    if (ptr != NULL) {
        fl_hdr *h = (fl_hdr *)((uint8_t *)ptr - sizeof(fl_hdr));
        size_t old_payload = (size_t)h->size - sizeof(fl_hdr);
        if (old_payload >= payload_need) {
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
        if (leftover >= sizeof(fl_hdr) + FL_SPLIT_MIN_TAIL_PAYLOAD) {
            fl_hdr *tail = (fl_hdr *)((uint8_t *)h + total_need);
            tail->size = leftover;
            tail->next = s_freelist;
            s_freelist = tail;
            h->size = total_need;
            s_freelist_splits++;
        }
        s_freelist_hits++;
        s_alloc_count++;
        if ((size_t)h->size > s_largest_satisfied) s_largest_satisfied = (size_t)h->size;
        return (uint8_t *)h + sizeof(fl_hdr);
    }

    /* Bump from heap_top. */
    if (heap_top + total_need > URBI_HEAP_BYTES) {
        s_last_failed_request = total_need;
        s_last_null_ptr_arg = NULL;
        s_last_null_nbytes_arg = nbytes;
#ifndef URBI_PORT_TEST
        /* v0.8.2 bring-up debug: dump heap stats every Nth OOM so we
         * can confirm UVM_STACK_CAP override is in effect + see whether
         * the freelist is recycling dead strand stacks.  Remove before
         * tag.  Throttled to ~1 in 32 OOMs to avoid UART drown. */
        static uint32_t s_oom_count = 0;
        if (((s_oom_count++) & 0x1FU) == 1U) {
            char b[100];
            const char *d = "0123456789ABCDEF";
            int n = 0;
            const char *t = "OOM req=";
            while (t[n] && n < 8) { b[n] = t[n]; n++; }
            for (int k = 28; k >= 0; k -= 4) b[n++] = d[(total_need >> k) & 0xF];
            const char *t2 = " top=";
            int j = 0; while (t2[j] && j < 5) { b[n++] = t2[j]; j++; }
            for (int k = 28; k >= 0; k -= 4) b[n++] = d[(heap_top >> k) & 0xF];
            const char *t3 = " fl=";
            j = 0; while (t3[j] && j < 4) { b[n++] = t3[j]; j++; }
            for (int k = 28; k >= 0; k -= 4) b[n++] = d[(s_freelist_hits >> k) & 0xF];
            const char *t4 = " frees=";
            j = 0; while (t4[j] && j < 7) { b[n++] = t4[j]; j++; }
            for (int k = 28; k >= 0; k -= 4) b[n++] = d[(s_free_count >> k) & 0xF];
            b[n++] = '\r'; b[n++] = '\n';
            port_writer(NULL, "oom", 3, b, (size_t)n, 0);
        }
#endif
        return NULL;
    }
    h = (fl_hdr *)&heap[heap_top];
    h->size = total_need;
    h->next = NULL;
    heap_top += total_need;
    s_alloc_count++;
    if (total_need > s_largest_satisfied) s_largest_satisfied = total_need;
    return (uint8_t *)h + sizeof(fl_hdr);
}
