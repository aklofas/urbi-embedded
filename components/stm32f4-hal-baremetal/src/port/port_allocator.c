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

/* v0.8.2 bring-up debug: distinguish leak vs alive-accumulation.
 * Track alive count per request-size bucket so we can see whether
 * specific sizes are growing unboundedly.  Five buckets cover the
 * urbi alloc footprint: <= 64 B (UObject slots / small structs),
 * <= 256 B (UClosure, UStrand, UProtoInstance), <= 1 KB (USlotArray,
 * UPropsTable, UProtos with several items), <= 16 KB (strand register
 * stacks — UVM_STACK_CAP * sizeof(UValue) lands in this bucket), and
 * > 16 KB (rare). */
#define ALLOC_BUCKET_COUNT 5
static const size_t s_bucket_max[ALLOC_BUCKET_COUNT] = {
    64U, 256U, 1024U, 16384U, (size_t)-1
};
static size_t s_alive_bytes = 0;
static size_t s_alloc_by_bucket[ALLOC_BUCKET_COUNT] = {0};
static size_t s_free_by_bucket [ALLOC_BUCKET_COUNT] = {0};
static int bucket_for(size_t total_block) {
    for (int i = 0; i < ALLOC_BUCKET_COUNT; i++) {
        if (total_block <= s_bucket_max[i]) return i;
    }
    return ALLOC_BUCKET_COUNT - 1;
}
size_t port_alloc_alive_bytes(void) { return s_alive_bytes; }
size_t port_alloc_alive_in_bucket(int b) {
    if (b < 0 || b >= ALLOC_BUCKET_COUNT) return 0;
    return s_alloc_by_bucket[b] - s_free_by_bucket[b];
}

/* v0.8.2 bring-up debug: count allocs of EXACTLY the strand register
 * stack size (UVM_STACK_CAP=512 × sizeof(UValue)=16 + 8 hdr = 8200 B
 * = 0x2008).  Helps confirm whether the 13-alive count in the 16K
 * bucket really is 13 strand stacks or includes other allocations
 * that happen to land in that bucket.  Remove before tag. */
static size_t s_stack_size_alloc = 0;
static size_t s_stack_size_free  = 0;
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
        fl_hdr *h = (fl_hdr *)((uint8_t *)ptr - sizeof(fl_hdr));
        s_alive_bytes -= h->size;
        s_free_by_bucket[bucket_for(h->size)]++;
        if (h->size == 0x2008U) s_stack_size_free++;
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
        s_alive_bytes += h->size;
        s_alloc_by_bucket[bucket_for(h->size)]++;
        if (h->size == 0x2008U) s_stack_size_alloc++;
        if ((size_t)h->size > s_largest_satisfied) s_largest_satisfied = (size_t)h->size;
        return (uint8_t *)h + sizeof(fl_hdr);
    }

    /* Bump from heap_top. */
    if (heap_top + total_need > URBI_HEAP_BYTES) {
        s_last_failed_request = total_need;
        s_last_null_ptr_arg = NULL;
        s_last_null_nbytes_arg = nbytes;
#ifndef URBI_PORT_TEST
        /* v0.8.2 bring-up debug: dump distinguishing diagnostic on every
         * 32nd OOM.  Layout (3 lines):
         *   [oom] req=R top=T alive_b=AB allocs=A frees=F fl_hits=H
         *   [oom] alive_64=N1 alive_256=N2 alive_1K=N3 alive_16K=N4 alive_>=16K=N5
         *   [oom] cum_64=C1 cum_256=C2 cum_1K=C3 cum_16K=C4 cum_>=16K=C5
         * Remove before tag. */
        static uint32_t s_oom_count = 0;
        if (((s_oom_count++) & 0x1FU) == 1U) {
            char b[160];
            const char *d = "0123456789ABCDEF";
            #define HEX32(val) do { \
                for (int k = 28; k >= 0; k -= 4) b[n++] = d[((uint32_t)(val) >> k) & 0xF]; \
            } while (0)
            #define LIT(s) do { const char *t = (s); int j=0; while (t[j]) b[n++] = t[j++]; } while (0)

            int n = 0;
            LIT("req=");      HEX32(total_need);
            LIT(" top=");     HEX32(heap_top);
            LIT(" alive_b="); HEX32(s_alive_bytes);
            LIT(" allocs=");  HEX32(s_alloc_count);
            LIT(" frees=");   HEX32(s_free_count);
            LIT(" fl=");      HEX32(s_freelist_hits);
            b[n++] = '\r'; b[n++] = '\n';
            port_writer(NULL, "oom", 3, b, (size_t)n, 0);

            n = 0;
            LIT("alive_64="); HEX32(s_alloc_by_bucket[0] - s_free_by_bucket[0]);
            LIT(" _256=");    HEX32(s_alloc_by_bucket[1] - s_free_by_bucket[1]);
            LIT(" _1K=");     HEX32(s_alloc_by_bucket[2] - s_free_by_bucket[2]);
            LIT(" _16K=");    HEX32(s_alloc_by_bucket[3] - s_free_by_bucket[3]);
            LIT(" _big=");    HEX32(s_alloc_by_bucket[4] - s_free_by_bucket[4]);
            b[n++] = '\r'; b[n++] = '\n';
            port_writer(NULL, "oom", 3, b, (size_t)n, 0);

            n = 0;
            LIT("cum_64=");   HEX32(s_alloc_by_bucket[0]);
            LIT(" _256=");    HEX32(s_alloc_by_bucket[1]);
            LIT(" _1K=");     HEX32(s_alloc_by_bucket[2]);
            LIT(" _16K=");    HEX32(s_alloc_by_bucket[3]);
            LIT(" _big=");    HEX32(s_alloc_by_bucket[4]);
            b[n++] = '\r'; b[n++] = '\n';
            port_writer(NULL, "oom", 3, b, (size_t)n, 0);

            /* Strand-stack-specific (size 0x2008): cumulative + alive */
            n = 0;
            LIT("stack8200 alive=");
            HEX32(s_stack_size_alloc - s_stack_size_free);
            LIT(" cum_alloc=");  HEX32(s_stack_size_alloc);
            LIT(" cum_free=");   HEX32(s_stack_size_free);
            b[n++] = '\r'; b[n++] = '\n';
            port_writer(NULL, "oom", 3, b, (size_t)n, 0);
            #undef HEX32
            #undef LIT
        }
#endif
        return NULL;
    }
    h = (fl_hdr *)&heap[heap_top];
    h->size = total_need;
    h->next = NULL;
    heap_top += total_need;
    s_alloc_count++;
    s_alive_bytes += total_need;
    s_alloc_by_bucket[bucket_for(total_need)]++;
    if (total_need == 0x2008U) s_stack_size_alloc++;
    if (total_need > s_largest_satisfied) s_largest_satisfied = total_need;
    return (uint8_t *)h + sizeof(fl_hdr);
}
