/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/ustrand.c — see rt/ustrand.h. */

#include "rt/ustrand.h"

UStrand *ustrand_new(struct UVM *vm, struct URealm *realm) {
    UStrand *s = (UStrand *)ugc_alloc(vm, UCELL_STRAND, sizeof(UStrand));
    if (!s) return NULL;
    s->vm = vm;
    s->realm = realm;
    s->id = ++uvm_objstats(vm)->next_id;
    s->state = USTRAND_PARKED;
    return s;
}

UClosure *uclosure_new(struct UVM *vm, UProto *proto, uint8_t nupvals) {
    size_t extra = nupvals ? (size_t)(nupvals - 1) * sizeof(UUpval *) : 0;
    UClosure *cl = (UClosure *)ugc_alloc(vm, UCELL_CLOSURE, sizeof(UClosure) + extra);
    if (!cl) return NULL;
    cl->proto = proto;
    cl->nupvals = nupvals;
    return cl;
}

int ustrand_ensure_stack(UStrand *s, uint32_t needed) {
    if (needed <= s->stack_cap) return 0;
    /* Eight, not thirty-two: a parked strand's register stack is the
     * single biggest thing it owns, and a hundred sleepers is a shape the
     * runtime is meant to be good at.  A thunk or a small function fits in
     * eight registers; anything larger doubles its way up in a handful of
     * reallocations, all of them on the strand's first frame. */
    uint32_t new_cap = s->stack_cap ? s->stack_cap : 8;
    while (new_cap < needed) new_cap *= 2;
    UValue *stack = (UValue *)ugc_raw_realloc(s->vm, s->stack, (size_t)s->stack_cap * sizeof(UValue), (size_t)new_cap * sizeof(UValue));
    if (!stack) return -1;
    s->stack = stack;
    s->stack_cap = new_cap;
    /* Registers are addressed as stack + index, never cached across a
     * call -- a realloc may move the block, so every open upvalue's ptr
     * (which points into it) must be rewritten here before anything else
     * touches the stack. */
    for (UUpval *u = s->open_upvals; u; u = u->next_open) u->ptr = s->stack + u->stack_index;
    return 0;
}

static int ustrand_grow_frames(UStrand *s) {
    uint16_t old_cap = s->frames_cap;
    uint16_t new_cap = old_cap ? (uint16_t)(old_cap * 2) : 4;
    UFrame *frames = (UFrame *)ugc_raw_realloc(s->vm, s->frames, (size_t)old_cap * sizeof(UFrame), (size_t)new_cap * sizeof(UFrame));
    if (!frames) return -1;
    s->frames = frames;
    s->frames_cap = new_cap;
    return 0;
}

int ustrand_push_frame(UStrand *s, UClosure *cl, UValue recv, uint32_t base, uint8_t ret_reg) {
    return ustrand_push_frame_args(s, cl, recv, base, ret_reg, 0);
}

int ustrand_push_frame_args(UStrand *s, UClosure *cl, UValue recv, uint32_t base,
                            uint8_t ret_reg, uint8_t nkeep) {
    if (s->nframes == s->frames_cap && ustrand_grow_frames(s) != 0) return -1;
    uint32_t max_reg = cl->proto ? cl->proto->max_reg : 0;
    uint32_t needed = base + max_reg + 1;
    if (ustrand_ensure_stack(s, needed) != 0) return -1;
    /* The window may already be within stack_cap from an earlier, since-
     * popped frame -- ensure_stack's zero-fill only covers newly grown
     * capacity, so this frame's own registers must be reset to nil here
     * regardless of whether growth just happened. */
    for (uint32_t i = base + nkeep; i < needed; i++) s->stack[i] = uv_nil();
    UFrame *f = &s->frames[s->nframes++];
    f->closure = cl;
    f->pc = cl->proto ? cl->proto->instructions : NULL;
    f->base = base;
    f->ret_reg = ret_reg;
    f->is_boundary = 0;
    f->recv = recv;
    return 0;
}

void ustrand_pop_frame(UStrand *s) {
    if (s->nframes == 0) return;
    const UFrame *f = &s->frames[s->nframes - 1];
    ustrand_close_upvals(s, f->base);
    s->nframes--;
}

int ustrand_push_cleanup(UStrand *s, UCleanup c) {
    if (s->ncleanup == s->cleanup_cap) {
        uint16_t new_cap = s->cleanup_cap ? (uint16_t)(s->cleanup_cap * 2) : 4;
        UCleanup *cleanup = (UCleanup *)ugc_raw_realloc(s->vm, s->cleanup, (size_t)s->cleanup_cap * sizeof(UCleanup), (size_t)new_cap * sizeof(UCleanup));
        if (!cleanup) return -1;
        s->cleanup = cleanup;
        s->cleanup_cap = new_cap;
    }
    s->cleanup[s->ncleanup++] = c;
    return 0;
}

UUpval *ustrand_find_or_open_upval(UStrand *s, uint32_t stack_index) {
    UUpval **pp = &s->open_upvals;
    while (*pp && (*pp)->stack_index > stack_index) pp = &(*pp)->next_open;
    /* The loop above also exits on *pp == NULL, so this guard is
     * load-bearing rather than a repeat of the loop condition. */
    /* cppcheck-suppress identicalInnerCondition */
    if (*pp && (*pp)->stack_index == stack_index) return *pp;
    UUpval *u = (UUpval *)ugc_alloc(s->vm, UCELL_UPVAL, sizeof(UUpval));
    if (!u) return NULL;
    u->stack_index = stack_index;
    u->ptr = s->stack + stack_index;
    u->next_open = *pp;
    *pp = u;
    return u;
}

void ustrand_close_upvals(UStrand *s, uint32_t from_index) {
    UUpval *u = s->open_upvals;
    while (u && u->stack_index >= from_index) {
        UUpval *next = u->next_open;
        u->closed = *u->ptr;
        u->ptr = &u->closed;
        u->next_open = NULL;
        u = next;
    }
    s->open_upvals = u;
}

void ustrand_trace(struct UVM *vm, UStrand *s) {
    /* The live register range is the maximum over every frame's window,
     * not just the top one's.  The top frame's top happens to dominate
     * today -- a callee's base is the caller's A + 1 or + 2, and the
     * emitter keeps no live value at or above a call's A -- but that is
     * an emitter invariant, not something this layer can see.  Taking the
     * max costs one pass over a handful of frames and removes the
     * dependency. */
    uint32_t top = 0;
    for (uint16_t i = 0; i < s->nframes; i++) {
        UFrame *f = &s->frames[i];
        uint32_t max_reg = (f->closure && f->closure->proto) ? f->closure->proto->max_reg : 0;
        uint32_t end = f->base + max_reg + 1;
        if (end > top) top = end;
    }
    if (top > s->stack_cap) top = s->stack_cap;
    for (uint32_t i = 0; i < top; i++) ugc_mark_value(vm, s->stack[i]);
    for (uint16_t i = 0; i < s->nframes; i++) {
        UFrame *f = &s->frames[i];
        if (f->closure) ugc_mark(vm, &f->closure->cell);
        ugc_mark_value(vm, f->recv);
    }
    ugc_mark_value(vm, s->transfer);
    ugc_mark_value(vm, s->result);
    /* tag/realm are opaque here but are cells by design -- mark the
     * pointer directly rather than through a field access. */
    if (s->tag) ugc_mark(vm, (UCell *)s->tag);
    if (s->realm) ugc_mark(vm, (UCell *)s->realm);
    for (UUpval *u = s->open_upvals; u; u = u->next_open) ugc_mark(vm, &u->cell);
    for (uint16_t i = 0; i < s->ncleanup; i++) {
        if (s->cleanup[i].tag) ugc_mark(vm, (UCell *)s->cleanup[i].tag);
        /* `saved` carries either the unwind value a finally body
         * suspended or the ambient tag a TAG_SCOPE displaced; both are
         * live and reachable from nowhere else, and it is nil otherwise. */
        ugc_mark_value(vm, s->cleanup[i].saved);
    }
    for (struct UCRoot *r = s->croots; r; r = r->prev) ugc_mark_value(vm, *r->slot);
    /* joiners is a wait list threaded via each waiting strand's own
     * `link` -- walking it here is traversal of that list, not treating
     * this strand's own link/next_in_realm/waiting_on as ownership edges
     * (those are scheduler linkage and are never marked through). */
    for (UStrand *j = s->joiners; j; j = j->link) ugc_mark(vm, &j->cell);
}

void ustrand_finalize(struct UVM *vm, UStrand *s) {
    ugc_raw_free(vm, s->stack, (size_t)s->stack_cap * sizeof(UValue));
    ugc_raw_free(vm, s->frames, (size_t)s->frames_cap * sizeof(UFrame));
    ugc_raw_free(vm, s->cleanup, (size_t)s->cleanup_cap * sizeof(UCleanup));
}
