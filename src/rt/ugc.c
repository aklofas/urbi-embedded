/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/ugc.c — see rt/ugc.h. */

#include "rt/ugc.h"
#include <string.h>

int ugc_init(UGc *g, UAllocFn alloc, void *ud) {
    memset(g, 0, sizeof *g);
    g->alloc = alloc; g->alloc_ud = ud;
    g->threshold = 16 * 1024; g->pause_ratio = 200;
    return 0;
}

void *ugc_raw_alloc(struct UVM *vm, size_t n) {
    UGc *g = uvm_gc(vm);
    void *p = g->alloc(NULL, n, g->alloc_ud);
    if (p) { memset(p, 0, n); g->bytes_since += n; g->bytes_live += n; g->raw_live += n; }
    return p;
}
void *ugc_raw_realloc(struct UVM *vm, void *p, size_t old, size_t n) {
    UGc *g = uvm_gc(vm);
    void *q = g->alloc(p, n, g->alloc_ud);
    if (q) {
        if (n > old) {
            memset((char *)q + old, 0, n - old);
            g->bytes_since += n - old; g->bytes_live += n - old; g->raw_live += n - old;
        } else {
            g->bytes_live -= old - n; g->raw_live -= old - n;
        }
    }
    return q;
}
void ugc_raw_free(struct UVM *vm, void *p, size_t n) {
    UGc *g = uvm_gc(vm);
    if (p) { g->alloc(p, 0, g->alloc_ud); g->bytes_live -= n; g->raw_live -= n; }
}

void *ugc_alloc(struct UVM *vm, UCellType type, size_t n) {
    UGc *g = uvm_gc(vm);
    ugc_maybe_collect(vm);
    UCell *c = (UCell *)g->alloc(NULL, n, g->alloc_ud);
    if (!c) { ugc_collect(vm); c = (UCell *)g->alloc(NULL, n, g->alloc_ud); if (!c) return NULL; }
    memset(c, 0, n);
    c->type = (uint8_t)type; c->size = (uint32_t)n;
    c->next = g->all; g->all = c;
    /* bytes_live is the live-set size as of the last collection (plus
     * raw_live, which is exact without a sweep) -- it is the pacing
     * baseline, so a fresh cell counts only toward bytes_since until the
     * next collect folds it in. Bumping bytes_live here too would make it
     * track bytes_since 1:1 and ugc_should_collect's 2x-live check could
     * never fire from allocation alone. */
    g->cells_live++; g->bytes_since += n;
    return c;
}

void ugc_mark(struct UVM *vm, UCell *c) {
    UGc *g = uvm_gc(vm);
    if (!c || c->marked) return;
    c->marked = 1;
    if (g->gray_len == g->gray_cap) {
        uint32_t nc = g->gray_cap ? g->gray_cap * 2 : 64;
        UCell **ng = (UCell **)g->alloc(g->gray, nc * sizeof *ng, g->alloc_ud);
        if (!ng) return;              /* mark stays set; cell traced conservatively as leaf */
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
    if (g->hooks.mark_fixed) g->hooks.mark_fixed(vm);
    while (g->gray_len) { UCell *c = g->gray[--g->gray_len]; if (g->hooks.trace) g->hooks.trace(vm, c); }
    UCell **pp = &g->all; size_t live = 0; uint32_t n = 0;
    while (*pp) {
        UCell *c = *pp;
        if (c->marked || (c->flags & UCELL_F_PINNED)) { c->marked = 0; live += c->size; n++; pp = &c->next; }
        else { *pp = c->next; if (g->hooks.finalize) g->hooks.finalize(vm, c); g->alloc(c, 0, g->alloc_ud); }
    }
    /* live counts only swept cell bytes; raw_live (arrays owned by the
     * surviving cells, freed via their finalize -> ugc_raw_free) must be
     * added back or every collect would silently forget live raw memory. */
    g->bytes_live = live + g->raw_live; g->cells_live = n; g->bytes_since = 0; g->cycles++;
    g->in_collect = 0;
}

bool ugc_should_collect(const UGc *g) {
    size_t limit = g->bytes_live * g->pause_ratio / 100;
    if (limit < g->threshold) limit = g->threshold;
    return g->bytes_since > limit;
}

void ugc_maybe_collect(struct UVM *vm) {
    UGc *g = uvm_gc(vm);
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
    if (g->gray) g->alloc(g->gray, 0, g->alloc_ud);
    g->gray = NULL; g->gray_cap = g->gray_len = 0;
}
