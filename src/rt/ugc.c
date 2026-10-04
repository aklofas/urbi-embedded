/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/ugc.c — see rt/ugc.h. */

#include "rt/ugc.h"
#include <string.h>

int ugc_init(UGc *g, UAllocFn alloc, void *ud) {
    memset(g, 0, sizeof *g);
    g->alloc = alloc; g->alloc_ud = ud;
    g->threshold = (size_t)16 * 1024; g->pause_ratio = 200;
    return 0;
}

void *ugc_raw_alloc(struct UVM *vm, size_t nbytes) {
    UGc *g = uvm_gc(vm);
    UGC_ASSERT(!g->in_collect);   /* finalize hooks must not allocate */
    void *p = g->alloc(NULL, nbytes, g->alloc_ud);
    if (!p) { g->collect_requested = 1; return NULL; }
    /* bytes_live follows the raw bytes exactly; pace_base does not move
     * until the next collection.  A baseline that grew with every raw
     * block would outrun bytes_since for any garbage whose raw block is at
     * least as big as its cell -- a list, an object with slots, a strand
     * -- and a loop making such garbage would never collect. */
    memset(p, 0, nbytes); g->bytes_since += nbytes; g->bytes_live += nbytes; g->raw_live += nbytes;
    return p;
}
void *ugc_raw_realloc(struct UVM *vm, void *p, size_t old, size_t nbytes) {
    UGc *g = uvm_gc(vm);
    void *q = g->alloc(p, nbytes, g->alloc_ud);
    if (!q) g->collect_requested = 1;
    if (q) {
        if (nbytes > old) {
            memset((char *)q + old, 0, nbytes - old);
            g->bytes_since += nbytes - old; g->bytes_live += nbytes - old; g->raw_live += nbytes - old;
        } else {
            g->bytes_live -= old - nbytes; g->raw_live -= old - nbytes;
        }
    }
    return q;
}
void ugc_raw_free(struct UVM *vm, void *p, size_t nbytes) {
    UGc *g = uvm_gc(vm);
    if (p) { g->alloc(p, 0, g->alloc_ud); g->bytes_live -= nbytes; g->raw_live -= nbytes; }
}

void *ugc_alloc(struct UVM *vm, UCellType type, size_t nbytes) {
    UGc *g = uvm_gc(vm);
    UGC_ASSERT(!g->in_collect);   /* finalize hooks must not allocate */
    ugc_maybe_collect(vm);
    UCell *c = (UCell *)g->alloc(NULL, nbytes, g->alloc_ud);
    if (!c) { ugc_collect(vm); c = (UCell *)g->alloc(NULL, nbytes, g->alloc_ud); if (!c) { g->collect_requested = 1; return NULL; } }
    memset(c, 0, nbytes);
    c->type = (uint8_t)type; c->size = (uint32_t)nbytes;
    c->next = g->all; g->all = c;
    /* bytes_live is the cell bytes that survived the last collection plus
     * raw_live, which is exact without a sweep; a fresh cell counts only
     * toward bytes_since until the next collect folds it in. */
    g->cells_live++; g->bytes_since += nbytes;
    return c;
}

void ugc_mark(struct UVM *vm, UCell *c) {
    UGc *g = uvm_gc(vm);
    if (!c || c->marked) return;      /* already gray(1) or black(2) */
    c->marked = 1;                    /* gray: queued, not yet traced */
    if (g->gray_len == g->gray_cap) {
        uint32_t nc = g->gray_cap ? g->gray_cap * 2 : 64;
        UCell **ng = (UCell **)g->alloc((void *)g->gray, (size_t)nc * sizeof(UCell *), g->alloc_ud);
        if (!ng) { g->gray_overflow = 1; return; }   /* stays gray; the post-drain
                                                       * fallback rescan in ugc_collect
                                                       * finds and traces it instead. */
        g->gray = ng; g->gray_cap = nc;
    }
    g->gray[g->gray_len++] = c;
}

void ugc_mark_value(struct UVM *vm, UValue v) {
    if (v.kind == UV_STR || v.kind == UV_OBJ || v.kind == UV_CELL) ugc_mark(vm, (UCell *)v.v.p);
}

void ugc_collect(struct UVM *vm) {
    UGc *g = uvm_gc(vm);
    if (g->in_collect) return;
    g->in_collect = 1;
    g->gray_len = 0;
    g->gray_overflow = 0;
    if (g->hooks.mark_fixed) g->hooks.mark_fixed(vm);
    /* A pinned cell survives sweep because THIS loop marks it: sweep keeps
     * any cell with a nonzero mark and knows nothing about the pin bits.
     * Going through ugc_mark rather than setting the mark directly is what
     * also traces the cell, so a child reachable only through a pinned
     * parent is not swept while the parent survives.  Both pins count --
     * the host's (urbi_ref) and the runtime's short-lived one. */
    for (UCell *c = g->all; c; c = c->next) if (c->flags & UCELL_F_PIN_ANY) ugc_mark(vm, c);
    /* Cells that couldn't be pushed onto gray[] (OOM growing it) are still
     * gray (marked == 1) but were never traced. Drain gray[] normally,
     * then -- if anything overflowed -- rescan the whole heap once for
     * leftover gray cells and trace them directly; that may push more
     * cells onto gray[] (their own children) or overflow again, so loop
     * back and drain fully before re-checking, rather than trusting the
     * rescan's own traversal order to reach what it just grayed. Without
     * this outer loop, a cell discovered only by the rescan would have its
     * *children* left gray-but-undrained past the end of this function --
     * harmless for a leaf (sweep keeps any nonzero mark) but wrong for
     * anything with further descendants. */
    for (;;) {
        while (g->gray_len) {
            UCell *c = g->gray[--g->gray_len];
            if (g->hooks.trace) g->hooks.trace(vm, c);
            c->marked = 2;             /* black: reached and traced */
        }
        if (!g->gray_overflow) break;
        g->gray_overflow = 0;
        for (UCell *c = g->all; c; c = c->next) {
            if (c->marked == 1) {
                if (g->hooks.trace) g->hooks.trace(vm, c);
                c->marked = 2;
            }
        }
    }
    UCell **pp = &g->all; size_t live = 0; uint32_t n = 0;
    while (*pp) {
        UCell *c = *pp;
        if (c->marked) { c->marked = 0; live += c->size; n++; pp = &c->next; }
        else { *pp = c->next; if (g->hooks.finalize) g->hooks.finalize(vm, c); g->alloc(c, 0, g->alloc_ud); }
    }
    /* live counts only swept cell bytes; raw_live (arrays owned by the
     * surviving cells, freed via their finalize -> ugc_raw_free) must be
     * added back or every collect would silently forget live raw memory. */
    g->bytes_live = live + g->raw_live; g->pace_base = g->bytes_live;
    g->cells_live = n; g->bytes_since = 0; g->cycles++;
    g->collect_requested = 0;
    g->in_collect = 0;
}

bool ugc_should_collect(const UGc *g) {
    if (g->collect_requested) return true;
    size_t limit = g->pace_base * g->pause_ratio / 100;
    if (limit < g->threshold) limit = g->threshold;
    if (g->heap_budget) {
        /* Collect before three quarters of the budget is reached.  What
         * is held counts the loaded chunks too: they come from the same
         * host heap, and the stdlib chunk alone is a large share of it.  The
         * floor keeps a nearly-full heap from collecting on every
         * allocation; past it the next refusal requests a collection. */
        size_t soft = g->heap_budget / 4 * 3;
        size_t held = g->pace_base + g->chunk_bytes;
        size_t room = soft > held ? soft - held : 0;
        size_t floor = g->heap_budget / 32;
        if (room < floor) room = floor;
        if (room < limit) limit = room;
    }
    return g->bytes_since > limit;
}

void ugc_maybe_collect(struct UVM *vm) {
    const UGc *g = uvm_gc(vm);
#ifdef URBI_GC_STRESS
    (void)g; ugc_collect(vm);
#else
    if (ugc_should_collect(g)) ugc_collect(vm);
#endif
}

void ugc_destroy(struct UVM *vm) {
    UGc *g = uvm_gc(vm);
    UCell *c = g->all;
    while (c) { UCell *next = c->next; if (g->hooks.finalize) g->hooks.finalize(vm, c); g->alloc(c, 0, g->alloc_ud); c = next; }
    g->all = NULL; g->cells_live = 0;
    if (g->gray) g->alloc((void *)g->gray, 0, g->alloc_ud);
    g->gray = NULL; g->gray_cap = g->gray_len = 0;
}
