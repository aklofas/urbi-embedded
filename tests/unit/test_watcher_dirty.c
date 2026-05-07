/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: watcher read-set capture, bit-6 lifecycle, observer_dirty,
 * watcher_eval_dirty + edge/level firing, pending_onleave_queue drain.
 * Row 11. */

#include "utest.h"
#include "vm/uvm.h"
#include "watcher/uwatcher.h"
#include "gc/ugc.h"            /* UTYPE_OBJECT */
#include "urbi/gc.h"       /* urbi_gc_alloc */
#include "gc/ugc_incremental.h" /* UGC_HAS_WATCHER_OBSERVER */
#include "tag/utag.h"           /* utag_create / utag_destroy */
#include "module/umodule.h"        /* UVAL_BOOL, UVAL_NIL */
#include "urbi/urbi.h"           /* urbi_tag_stop, URBI_LOG_WARN */

#include <stdlib.h>   /* realloc / free — test-side only; NOT in src/ */
#include <stddef.h>
#include <stdbool.h>

#define UTEST(name) static void name(void)

/* === Test cases === */

/* 1. watcher_install_sets_bit6:
 *    Install one watcher with a 1-cell read-set; verify bit-6 is set on the
 *    cell.  Unregister; verify bit-6 is cleared. */
UTEST(watcher_install_sets_bit6)
{
    UVM   vm;
    UCell *c;
    UCell *rs[1];
    UWatcher *w;

    uvm_init(&vm, NULL, NULL);

    c = urbi_gc_alloc(&vm, 64u, UTYPE_OBJECT);
    UASSERT(c != NULL);

    rs[0] = c;
    w = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, NULL, NULL, NULL, rs, 1u);
    UASSERT(w != NULL);

    /* Bit-6 must be set on the observed cell. */
    UASSERT(c->gc_byte & UGC_HAS_WATCHER_OBSERVER);
    /* cells[0] must point to c. */
    UASSERT(w->cells[0] == c);
    UASSERT_EQ((unsigned)w->read_set_count, 1u);

    /* Unregister — no other watcher observes c, so bit-6 must be cleared. */
    urbi_watcher_unregister_internal(&vm, w);
    UASSERT(!(c->gc_byte & UGC_HAS_WATCHER_OBSERVER));

    uvm_destroy(&vm);
}

/* 2. watcher_overlap_keeps_bit6_until_last:
 *    Two watchers share one cell in their read-sets.
 *    Unregistering the first watcher must NOT clear bit-6 (the second still
 *    observes the cell).  Unregistering the second must clear bit-6. */
UTEST(watcher_overlap_keeps_bit6_until_last)
{
    UVM   vm;
    UCell *c;
    UCell *rs[1];
    UWatcher *w1, *w2;

    uvm_init(&vm, NULL, NULL);

    c = urbi_gc_alloc(&vm, 64u, UTYPE_OBJECT);
    UASSERT(c != NULL);

    rs[0] = c;
    w1 = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, NULL, NULL, NULL, rs, 1u);
    w2 = urbi_watcher_install_internal(
        &vm, UWATCHER_WHENEVER, NULL, NULL, NULL, NULL, rs, 1u);
    UASSERT(w1 != NULL);
    UASSERT(w2 != NULL);

    /* Both observe c — bit-6 set. */
    UASSERT(c->gc_byte & UGC_HAS_WATCHER_OBSERVER);

    /* Unregister w1; w2 still observes c — bit-6 must remain. */
    urbi_watcher_unregister_internal(&vm, w1);
    UASSERT(c->gc_byte & UGC_HAS_WATCHER_OBSERVER);

    /* Unregister w2 — no observers left — bit-6 must be cleared. */
    urbi_watcher_unregister_internal(&vm, w2);
    UASSERT(!(c->gc_byte & UGC_HAS_WATCHER_OBSERVER));

    uvm_destroy(&vm);
}

/* 3. watcher_install_inserts_into_tag_member_list:
 *    Install a watcher with a non-NULL owning_tag; verify it appears at
 *    member_watchers_head.  Unregister; verify member_watchers_head is NULL. */
UTEST(watcher_install_inserts_into_tag_member_list)
{
    UVM      vm;
    UTag    *tag;
    UWatcher *w;

    uvm_init(&vm, NULL, NULL);

    tag = utag_create(&vm);
    UASSERT(tag != NULL);

    w = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, tag, NULL, NULL, NULL, NULL, 0u);
    UASSERT(w != NULL);

    /* Watcher must be head of tag's member list. */
    UASSERT(tag->member_watchers_head == w);
    UASSERT(w->next_in_tag == NULL);

    urbi_watcher_unregister_internal(&vm, w);
    UASSERT(tag->member_watchers_head == NULL);

    utag_destroy(&vm, tag);
    uvm_destroy(&vm);
}

/* 4. observer_dirty_bumps_counter:
 *    Directly call observer_dirty and verify watcher_dirty_count increments. */
UTEST(observer_dirty_bumps_counter)
{
    UVM   vm;
    UCell c;

    uvm_init(&vm, NULL, NULL);

    /* Zero out the dummy cell header enough for the call. */
    c.type_tag = UTYPE_OBJECT;
    c.gc_byte  = 0;

    uint32_t before = vm.watcher_dirty_count;
    observer_dirty(&vm, &c, 42u);
    UASSERT_EQ((long long)vm.watcher_dirty_count, (long long)(before + 1u));

    uvm_destroy(&vm);
}

/* 5. watcher_active_count_tracks_install_unregister:
 *    Install N watchers; verify counter == N.
 *    Unregister all; verify counter returns to 0. */
UTEST(watcher_active_count_tracks_install_unregister)
{
    UVM   vm;
    UWatcher *w[3];
    int i;

    uvm_init(&vm, NULL, NULL);

    UASSERT_EQ((long long)vm.watcher_active_count, 0LL);

    for (i = 0; i < 3; i++) {
        w[i] = urbi_watcher_install_internal(
            &vm, UWATCHER_AT, NULL, NULL, NULL, NULL, NULL, 0u);
        UASSERT(w[i] != NULL);
    }
    UASSERT_EQ((long long)vm.watcher_active_count, 3LL);

    for (i = 0; i < 3; i++) {
        urbi_watcher_unregister_internal(&vm, w[i]);
    }
    UASSERT_EQ((long long)vm.watcher_active_count, 0LL);

    uvm_destroy(&vm);
}

/* 6. active_list_is_tail_inserted:
 *    Install 3 watchers w1, w2, w3; walk active_watchers_head and verify the
 *    order matches install order (tail insertion per row 12 §2.2). */
UTEST(active_list_is_tail_inserted)
{
    UVM   vm;
    UWatcher *w1, *w2, *w3;
    UWatcher *cur;

    uvm_init(&vm, NULL, NULL);

    w1 = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, NULL, NULL, NULL, NULL, 0u);
    w2 = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, NULL, NULL, NULL, NULL, 0u);
    w3 = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, NULL, NULL, NULL, NULL, 0u);
    UASSERT(w1 != NULL && w2 != NULL && w3 != NULL);

    /* Walk active list: must be w1 → w2 → w3 → NULL. */
    cur = vm.active_watchers_head;
    UASSERT(cur == w1);  cur = cur->next_active;
    UASSERT(cur == w2);  cur = cur->next_active;
    UASSERT(cur == w3);  cur = cur->next_active;
    UASSERT(cur == NULL);

    urbi_watcher_unregister_internal(&vm, w1);
    urbi_watcher_unregister_internal(&vm, w2);
    urbi_watcher_unregister_internal(&vm, w3);

    uvm_destroy(&vm);
}

/* 7. watcher_install_readset_overflow_returns_null:
 *    Pass read_set_count > URBI_WATCHER_READSET_MAX; install must return NULL
 *    and pool_in_use must remain 0 (no slot wasted). */
UTEST(watcher_install_readset_overflow_returns_null)
{
    UVM   vm;
    UWatcher *w;
    UCell *dummy[URBI_WATCHER_READSET_MAX + 1];
    size_t i;

    uvm_init(&vm, NULL, NULL);

    /* Fill dummy array with non-NULL pointers (values don't matter; install
     * must reject before dereferencing them). */
    for (i = 0; i <= (size_t)URBI_WATCHER_READSET_MAX; i++) {
        dummy[i] = (UCell *)&dummy[i]; /* self-referential; non-NULL */
    }

    w = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, NULL, NULL, NULL,
        dummy, (size_t)URBI_WATCHER_READSET_MAX + 1u);

    UASSERT(w == NULL);
    /* Pool must be untouched — no slot consumed. */
    UASSERT_EQ((int)vm.watcher_pool_in_use, 0);

    uvm_destroy(&vm);
}

/* ===================================================================
 * T34 test cases: watcher_eval_dirty + edge/level firing
 * =================================================================== */

/* --- Helpers for condition hook --- */

/* Static counter used by toggling condition hook below. */
static int g_condition_toggle_step;
static int g_condition_truthy;

static UValue
condition_hook_fixed_false(struct UVM *vm, struct UWatcher *w)
{
    UValue v = {0};  /* UVAL_NIL = falsy */
    (void)vm; (void)w;
    return v;
}

static UValue
condition_hook_fixed_true(struct UVM *vm, struct UWatcher *w)
{
    UValue v;
    (void)vm; (void)w;
    v.kind   = UVAL_BOOL;
    v.v.i    = 1;
    return v;
}

static UValue
condition_hook_toggle(struct UVM *vm, struct UWatcher *w)
{
    UValue v = {0};  /* default falsy */
    (void)vm; (void)w;
    if (g_condition_truthy) {
        v.kind = UVAL_BOOL;
        v.v.i  = 1;
    }
    return v;
}

/* Fire counter bumped by test_watcher_fire_hook. */
static int g_fire_count;

static void
fire_hook_count(struct UVM *vm, struct UWatcher *w)
{
    (void)vm; (void)w;
    g_fire_count++;
}

/* 8. watcher_eval_dirty_skips_when_count_zero:
 *    Eval with no dirty count set — must not crash, must leave in_watcher_eval 0. */
UTEST(watcher_eval_dirty_skips_when_count_zero)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UASSERT_EQ((int)vm.watcher_dirty_count, 0);
    watcher_eval_dirty(&vm);
    UASSERT(!vm.in_watcher_eval);

    uvm_destroy(&vm);
}

/* 9. watcher_eval_dirty_resets_count_to_zero:
 *    Set dirty count to 5; eval must clear it (and leave in_watcher_eval 0). */
UTEST(watcher_eval_dirty_resets_count_to_zero)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    vm.watcher_dirty_count = 5u;
    watcher_eval_dirty(&vm);
    UASSERT_EQ((int)vm.watcher_dirty_count, 0);
    UASSERT(!vm.in_watcher_eval);

    uvm_destroy(&vm);
}

/* 10. watcher_eval_at_edge_only_fires_on_false_to_true:
 *     Install AT watcher.  Three dirty-eval passes with controlled condition.
 *     Pass 1: condition false → no fire (last_cache seeded false at install).
 *     Pass 2: condition true  → fire (false→true edge).
 *     Pass 3: condition true  → no fire (true→true, no edge). */
UTEST(watcher_eval_at_edge_only_fires_on_false_to_true)
{
    UVM vm;
    UWatcher *w;

    uvm_init(&vm, NULL, NULL);

    g_fire_count       = 0;
    g_condition_truthy = 0;  /* start false */

    vm.test_watcher_condition_hook = condition_hook_toggle;
    vm.test_watcher_fire_hook      = fire_hook_count;

    w = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, /*condition=*/(UClosure *)1,
        NULL, NULL, NULL, 0u);
    UASSERT(w != NULL);
    /* Install-time seed with false: condition_hook_toggle returns UVAL_NIL
     * (kind=0) when g_condition_truthy=0.  Verify the cache is falsy. */
    UASSERT_EQ((int)w->last_value_cache.kind, (int)UVAL_NIL);

    /* Pass 1: still false → no fire. */
    vm.watcher_dirty_count = 1u;
    watcher_eval_dirty(&vm);
    UASSERT_EQ(g_fire_count, 0);

    /* Pass 2: flip to true → edge fires. */
    g_condition_truthy = 1;
    vm.watcher_dirty_count = 1u;
    watcher_eval_dirty(&vm);
    UASSERT_EQ(g_fire_count, 1);

    /* Pass 3: still true → no fire (no rising edge). */
    vm.watcher_dirty_count = 1u;
    watcher_eval_dirty(&vm);
    UASSERT_EQ(g_fire_count, 1);

    urbi_watcher_unregister_internal(&vm, w);
    uvm_destroy(&vm);
}

/* 11. watcher_eval_whenever_level_fires_each_dirty_pass:
 *     Install WHENEVER with always-true condition; three dirty-eval passes;
 *     fire counter must be 3. */
UTEST(watcher_eval_whenever_level_fires_each_dirty_pass)
{
    UVM vm;
    UWatcher *w;

    uvm_init(&vm, NULL, NULL);

    g_fire_count = 0;

    vm.test_watcher_condition_hook = condition_hook_fixed_true;
    vm.test_watcher_fire_hook      = fire_hook_count;

    w = urbi_watcher_install_internal(
        &vm, UWATCHER_WHENEVER, NULL, /*condition=*/(UClosure *)1,
        NULL, NULL, NULL, 0u);
    UASSERT(w != NULL);

    /* Three passes — WHENEVER fires every time condition is truthy. */
    vm.watcher_dirty_count = 1u; watcher_eval_dirty(&vm);
    vm.watcher_dirty_count = 1u; watcher_eval_dirty(&vm);
    vm.watcher_dirty_count = 1u; watcher_eval_dirty(&vm);
    UASSERT_EQ(g_fire_count, 3);

    urbi_watcher_unregister_internal(&vm, w);
    uvm_destroy(&vm);
}

/* 12. watcher_eval_skips_pending_unregister:
 *     Set URBI_WATCHER_PENDING_UNREGISTER on watcher; verify no fire and
 *     last_value_cache unchanged after eval. */
UTEST(watcher_eval_skips_pending_unregister)
{
    UVM vm;
    UWatcher *w;
    UValue initial_cache;

    uvm_init(&vm, NULL, NULL);

    g_fire_count = 0;

    vm.test_watcher_condition_hook = condition_hook_fixed_true;
    vm.test_watcher_fire_hook      = fire_hook_count;

    w = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, /*condition=*/(UClosure *)1,
        NULL, NULL, NULL, 0u);
    UASSERT(w != NULL);

    initial_cache = w->last_value_cache;

    /* Mark pending-unregister before eval. */
    w->flags |= URBI_WATCHER_PENDING_UNREGISTER;

    vm.watcher_dirty_count = 1u;
    watcher_eval_dirty(&vm);

    /* No fire, last_value_cache unchanged. */
    UASSERT_EQ(g_fire_count, 0);
    UASSERT_EQ((int)w->last_value_cache.kind, (int)initial_cache.kind);

    /* Manual cleanup (watcher has PENDING_UNREGISTER; unregister normally). */
    w->flags &= (uint8_t)~(uint8_t)URBI_WATCHER_PENDING_UNREGISTER;
    urbi_watcher_unregister_internal(&vm, w);
    uvm_destroy(&vm);
}

/* 13. watcher_install_seeds_last_value_cache:
 *     Install AT watcher with always-true condition hook; verify that the
 *     install-time seed captures the true value, and that a subsequent eval
 *     pass with the same true condition does NOT fire (no rising edge since
 *     old and new are both truthy). */
UTEST(watcher_install_seeds_last_value_cache)
{
    UVM vm;
    UWatcher *w;

    uvm_init(&vm, NULL, NULL);

    g_fire_count = 0;

    vm.test_watcher_condition_hook = condition_hook_fixed_true;
    vm.test_watcher_fire_hook      = fire_hook_count;

    w = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, /*condition=*/(UClosure *)1,
        NULL, NULL, NULL, 0u);
    UASSERT(w != NULL);
    /* Install-time seed must be truthy. */
    UASSERT(w->last_value_cache.kind == UVAL_BOOL && w->last_value_cache.v.i == 1);

    /* First dirty pass: old=true, new=true → no rising edge → no fire. */
    vm.watcher_dirty_count = 1u;
    watcher_eval_dirty(&vm);
    UASSERT_EQ(g_fire_count, 0);

    urbi_watcher_unregister_internal(&vm, w);
    uvm_destroy(&vm);
}

/* (former case 14 — watcher_scratch_frame_allocated_at_init — removed by
 * WATCH-022 in Wave-1 cleanup.  The UScratchFrame heap allocation it
 * defended is gone; v0.5.1-cond-unstub routed scratch frames onto the
 * C stack via urbi_run_closure_on_scratch.) */

/* ===================================================================
 * T35 test cases: pending_onleave_queue drain + run_watcher_onleave
 * =================================================================== */

/* --- Helpers for onleave hook --- */

static int g_onleave_count;

/* Records the order in which onleave is called for FIFO ordering test. */
static UWatcher *g_onleave_order[8];
static int       g_onleave_order_idx;

static void
onleave_hook_count(struct UVM *vm, struct UWatcher *w)
{
    (void)vm;
    if (g_onleave_order_idx < 8) {
        g_onleave_order[g_onleave_order_idx++] = w;
    }
    g_onleave_count++;
}

/* 15. pending_onleave_push_sets_flag_and_unlinks_from_active:
 *     Install one watcher; push to pending_onleave_queue; verify
 *     URBI_WATCHER_PENDING_UNREGISTER is set, watcher is off active_watchers_head,
 *     and pending_onleave_head == the watcher. */
UTEST(pending_onleave_push_sets_flag_and_unlinks_from_active)
{
    UVM      vm;
    UWatcher *w;
    UTag     *tag;

    uvm_init(&vm, NULL, NULL);

    tag = utag_create(&vm);
    UASSERT(tag != NULL);

    w = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, tag, NULL, NULL, NULL, NULL, 0u);
    UASSERT(w != NULL);
    UASSERT(vm.active_watchers_head == w);
    UASSERT(tag->member_watchers_head == w);

    pending_onleave_queue_push(&vm, w);

    /* Flag set. */
    UASSERT(w->flags & URBI_WATCHER_PENDING_UNREGISTER);
    /* Unlinked from active list. */
    UASSERT(vm.active_watchers_head == NULL);
    /* Unlinked from tag member list. */
    UASSERT(tag->member_watchers_head == NULL);
    /* Head and tail of pending queue point to w. */
    UASSERT(vm.pending_onleave_head == w);
    UASSERT(vm.pending_onleave_tail == w);
    UASSERT(w->next_active == NULL);
    /* watcher_active_count NOT decremented at push — still 1. */
    UASSERT_EQ((long long)vm.watcher_active_count, 1LL);

    /* Drain to clean up (unregisters the watcher). */
    drain_pending_onleave_queue(&vm);
    UASSERT(vm.pending_onleave_head == NULL);
    UASSERT_EQ((long long)vm.watcher_active_count, 0LL);

    utag_destroy(&vm, tag);
    uvm_destroy(&vm);
}

/* 16. pending_onleave_drain_walks_until_empty:
 *     Install 5 watchers; push all to pending_onleave_queue; drain;
 *     assert head/tail NULL and pool_in_use == 0. */
UTEST(pending_onleave_drain_walks_until_empty)
{
    UVM      vm;
    UWatcher *w[5];
    int       i;

    uvm_init(&vm, NULL, NULL);

    for (i = 0; i < 5; i++) {
        w[i] = urbi_watcher_install_internal(
            &vm, UWATCHER_AT, NULL, NULL, NULL, NULL, NULL, 0u);
        UASSERT(w[i] != NULL);
    }
    UASSERT_EQ((long long)vm.watcher_active_count, 5LL);
    UASSERT_EQ((int)vm.watcher_pool_in_use, 5);

    for (i = 0; i < 5; i++) {
        pending_onleave_queue_push(&vm, w[i]);
    }

    drain_pending_onleave_queue(&vm);

    UASSERT(vm.pending_onleave_head == NULL);
    UASSERT(vm.pending_onleave_tail == NULL);
    UASSERT_EQ((long long)vm.watcher_active_count, 0LL);
    UASSERT_EQ((int)vm.watcher_pool_in_use, 0);
    UASSERT(!vm.in_watcher_eval);

    uvm_destroy(&vm);
}

/* 17. pending_onleave_drain_invokes_hook_when_onleave_set:
 *     Install watcher with non-NULL onleave field; push; drain;
 *     verify onleave hook called exactly once. */
UTEST(pending_onleave_drain_invokes_hook_when_onleave_set)
{
    UVM      vm;
    UWatcher *w;

    uvm_init(&vm, NULL, NULL);

    g_onleave_count     = 0;
    g_onleave_order_idx = 0;
    vm.test_watcher_onleave_hook = onleave_hook_count;

    /* Pass a non-NULL onleave pointer so run_watcher_onleave is entered.
     * The pointer value doesn't matter at M3 — only non-NULL triggers the hook path. */
    w = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, NULL, NULL,
        /*onleave=*/(UClosure *)1, NULL, 0u);
    UASSERT(w != NULL);

    pending_onleave_queue_push(&vm, w);
    drain_pending_onleave_queue(&vm);

    UASSERT_EQ(g_onleave_count, 1);
    UASSERT(vm.pending_onleave_head == NULL);

    uvm_destroy(&vm);
}

/* 18. pending_onleave_drain_skips_null_onleave:
 *     Install watcher with onleave=NULL; push; drain; verify hook NOT called
 *     and watcher is properly unregistered. */
UTEST(pending_onleave_drain_skips_null_onleave)
{
    UVM      vm;
    UWatcher *w;

    uvm_init(&vm, NULL, NULL);

    g_onleave_count = 0;
    vm.test_watcher_onleave_hook = onleave_hook_count;

    w = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, NULL, NULL,
        /*onleave=*/NULL, NULL, 0u);
    UASSERT(w != NULL);

    pending_onleave_queue_push(&vm, w);
    drain_pending_onleave_queue(&vm);

    /* Hook must NOT be called when onleave is NULL. */
    UASSERT_EQ(g_onleave_count, 0);
    UASSERT(vm.pending_onleave_head == NULL);
    UASSERT_EQ((int)vm.watcher_pool_in_use, 0);

    uvm_destroy(&vm);
}

/* 19. pending_onleave_drain_ordering_FIFO:
 *     Push watchers A, B, C in order; drain; verify onleave hook saw A then B then C. */
UTEST(pending_onleave_drain_ordering_FIFO)
{
    UVM      vm;
    UWatcher *wa, *wb, *wc;

    uvm_init(&vm, NULL, NULL);

    g_onleave_count     = 0;
    g_onleave_order_idx = 0;
    vm.test_watcher_onleave_hook = onleave_hook_count;

    wa = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, NULL, NULL, (UClosure *)1, NULL, 0u);
    wb = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, NULL, NULL, (UClosure *)1, NULL, 0u);
    wc = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, NULL, NULL, (UClosure *)1, NULL, 0u);
    UASSERT(wa != NULL && wb != NULL && wc != NULL);

    pending_onleave_queue_push(&vm, wa);
    pending_onleave_queue_push(&vm, wb);
    pending_onleave_queue_push(&vm, wc);

    drain_pending_onleave_queue(&vm);

    UASSERT_EQ(g_onleave_order_idx, 3);
    UASSERT(g_onleave_order[0] == wa);
    UASSERT(g_onleave_order[1] == wb);
    UASSERT(g_onleave_order[2] == wc);

    uvm_destroy(&vm);
}

/* 20. tag_stop_pushes_watchers_to_onleave_queue:
 *     Install a watcher on a tag; call urbi_tag_stop on that tag;
 *     verify the watcher ends up on the pending_onleave_queue.
 *     (Tests the urbi_tag_stop cascade path in uunwind.c.) */
UTEST(tag_stop_pushes_watchers_to_onleave_queue)
{
    UVM      vm;
    UTag    *tag;
    UWatcher *w;
    UValue    nil;

    uvm_init(&vm, NULL, NULL);

    nil.kind = UVAL_NIL;
    nil.v.i  = 0;

    tag = utag_create(&vm);
    UASSERT(tag != NULL);

    w = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, tag, NULL, NULL, NULL, NULL, 0u);
    UASSERT(w != NULL);
    UASSERT(tag->member_watchers_head == w);

    /* urbi_tag_stop(vm, tag, value): iterates member_strands_head (empty here
     * — no strands to deposit on), then walks member_watchers_head (the cascade
     * we are testing).  No strands needed; cascade fires unconditionally. */
    urbi_tag_stop(&vm, tag, nil);

    /* After tag_stop, the watcher should be on the pending_onleave_queue. */
    UASSERT(vm.pending_onleave_head == w);
    /* tag's member_watchers_head cleared by push. */
    UASSERT(tag->member_watchers_head == NULL);

    /* Drain to clean up. */
    drain_pending_onleave_queue(&vm);
    UASSERT(vm.pending_onleave_head == NULL);

    utag_destroy(&vm, tag);
    uvm_destroy(&vm);
}

/* ===================================================================
 * T36 test cases: GC root walker for watchers + spawn_body relocation
 * =================================================================== */

/* --- Helper: count how many times the GC root callback is invoked --- */

static void
root_cb_count(struct UVM *vm, UValue *slot, void *ctx)
{
    (void)vm; (void)slot;
    int *count = (int *)ctx;
    (*count)++;
}

/* 21. watcher_root_walker_visits_active_watchers:
 *     Install one AT watcher with non-NULL condition, body, and onleave.
 *     Call urbi_gc_walk_roots; verify callback was invoked at least 3 times
 *     for the watcher (condition + body + onleave closures + last_value_cache). */
UTEST(watcher_root_walker_visits_active_watchers)
{
    UVM      vm;
    UWatcher *w;
    int       count_before;
    int       count_after;
    int       total;

    uvm_init(&vm, NULL, NULL);

    /* Use non-NULL pointer sentinels for closures (cast; value not dereferenced
     * by the GC walk itself — the GC mark callback only receives UValue pointers,
     * and our counting stub ignores the value). */
    w = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL,
        /*condition=*/(UClosure *)1,
        /*body=*/     (UClosure *)2,
        /*onleave=*/  (UClosure *)3,
        NULL, 0u);
    UASSERT(w != NULL);

    count_before = 0;
    urbi_gc_walk_roots(&vm, root_cb_count, &count_before);

    /* Watcher must have contributed: condition(1) + body(1) + onleave(1) +
     * last_value_cache(1) = 4 calls minimum.  Other root providers may add more. */
    UASSERT(count_before >= 4);

    /* Unregister and walk again; the delta should drop by at least 4. */
    urbi_watcher_unregister_internal(&vm, w);

    count_after = 0;
    urbi_gc_walk_roots(&vm, root_cb_count, &count_after);

    total = count_before - count_after;
    UASSERT(total >= 4);

    uvm_destroy(&vm);
}

/* 22. watcher_root_walker_visits_pending_onleave:
 *     Install watcher with non-NULL onleave; push to pending_onleave_queue;
 *     walk roots; assert callback invoked at least once for that watcher
 *     (onleave closure + last_value_cache). */
UTEST(watcher_root_walker_visits_pending_onleave)
{
    UVM      vm;
    UWatcher *w;
    int       count_active;
    int       count_pending;

    uvm_init(&vm, NULL, NULL);

    w = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL,
        /*condition=*/NULL,
        /*body=*/     NULL,
        /*onleave=*/  (UClosure *)1,
        NULL, 0u);
    UASSERT(w != NULL);

    /* Walk with watcher on active list. */
    count_active = 0;
    urbi_gc_walk_roots(&vm, root_cb_count, &count_active);

    /* Move watcher to pending_onleave_queue. */
    pending_onleave_queue_push(&vm, w);

    /* Walk with watcher on pending queue — should still see onleave + last_value_cache. */
    count_pending = 0;
    urbi_gc_walk_roots(&vm, root_cb_count, &count_pending);

    /* pending queue walker contributes at minimum: onleave(1) + last_value_cache(1) = 2. */
    UASSERT(count_pending >= 2);

    /* Drain to clean up.  Install a no-op onleave hook so run_watcher_onleave
     * doesn't dispatch the (UClosure *)1 sentinel through real bytecode. */
    vm.test_watcher_onleave_hook = onleave_hook_count;
    drain_pending_onleave_queue(&vm);

    uvm_destroy(&vm);
}

/* 23. watcher_root_walker_skips_null_closures:
 *     Install watcher with all closures NULL.  Walk roots; the watcher
 *     contributes exactly 1 call (last_value_cache only) per active-list pass. */
UTEST(watcher_root_walker_skips_null_closures)
{
    UVM      vm;
    UWatcher *w;
    int       count_with;
    int       count_without;
    int       delta;

    uvm_init(&vm, NULL, NULL);

    /* Walk roots with no watchers to get a baseline. */
    count_without = 0;
    urbi_gc_walk_roots(&vm, root_cb_count, &count_without);

    w = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL,
        /*condition=*/NULL,
        /*body=*/     NULL,
        /*onleave=*/  NULL,
        NULL, 0u);
    UASSERT(w != NULL);

    count_with = 0;
    urbi_gc_walk_roots(&vm, root_cb_count, &count_with);

    /* Delta must be exactly 1 (only last_value_cache; no closure callbacks). */
    delta = count_with - count_without;
    UASSERT_EQ(delta, 1);

    urbi_watcher_unregister_internal(&vm, w);
    uvm_destroy(&vm);
}

/* 24. watcher_root_provider_count_is_6_after_init:
 *     Verify 6 root providers are registered at uvm_init time
 *     (sched, realm, intern, host_handle, watcher_table, T36 m4_object). */
UTEST(watcher_root_provider_count_is_6_after_init)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    UASSERT_EQ(6u, vm.root_provider_count);
    uvm_destroy(&vm);
}

/* 25. spawn_body_coroutine_relocated_still_works:
 *     After moving spawn_body_coroutine to uwatcher_spawn.c, verify
 *     the test_watcher_fire_hook delegation still works as before.
 *
 *     Install without condition hook so seed is nil (falsy).  Then set
 *     the condition hook to return true on first dirty eval — this gives a
 *     false→true rising edge that fires spawn_body_coroutine via fire hook. */
UTEST(spawn_body_coroutine_relocated_still_works)
{
    UVM      vm;
    UWatcher *w;

    uvm_init(&vm, NULL, NULL);

    g_fire_count = 0;

    /* Install with no condition hook so seed = UVAL_NIL (falsy). */
    w = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, (UClosure *)1,
        NULL, NULL, NULL, 0u);
    UASSERT(w != NULL);
    /* Confirm seed is NIL/falsy. */
    UASSERT(w->last_value_cache.kind == UVAL_NIL);

    /* Now set the condition hook (returns true) and the fire hook. */
    vm.test_watcher_condition_hook = condition_hook_fixed_true;
    vm.test_watcher_fire_hook      = fire_hook_count;

    /* Trigger one dirty eval — rising edge: nil→true fires once. */
    vm.watcher_dirty_count = 1u;
    watcher_eval_dirty(&vm);
    UASSERT_EQ(g_fire_count, 1);

    urbi_watcher_unregister_internal(&vm, w);
    uvm_destroy(&vm);
}

/* ===================================================================
 * §9.2 gap-fill: eval_pass_walks_all_watchers
 * =================================================================== */

/* 26. eval_pass_walks_all_watchers:
 *     Install 3 watchers; trigger one dirty-eval pass; assert all 3 have
 *     their condition evaluated (fire_count reflects all 3 seeing a
 *     false→true edge). */
UTEST(eval_pass_walks_all_watchers)
{
    UVM      vm;
    UWatcher *w1, *w2, *w3;

    uvm_init(&vm, NULL, NULL);

    g_fire_count = 0;

    /* All three: AT mode, no seed (install with no condition hook → NIL cache),
     * then set condition hook to fixed_true before eval so all see false→true. */
    w1 = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, (UClosure *)1, NULL, NULL, NULL, 0u);
    w2 = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, (UClosure *)1, NULL, NULL, NULL, 0u);
    w3 = urbi_watcher_install_internal(
        &vm, UWATCHER_AT, NULL, (UClosure *)1, NULL, NULL, NULL, 0u);
    UASSERT(w1 != NULL && w2 != NULL && w3 != NULL);

    /* Seeds are NIL (falsy) because no condition hook was set during install. */
    UASSERT_EQ((int)w1->last_value_cache.kind, (int)UVAL_NIL);
    UASSERT_EQ((int)w2->last_value_cache.kind, (int)UVAL_NIL);
    UASSERT_EQ((int)w3->last_value_cache.kind, (int)UVAL_NIL);

    /* Set hooks to fire: condition_hook_fixed_true returns truthy;
     * fire_hook_count increments g_fire_count once per fired watcher. */
    vm.test_watcher_condition_hook = condition_hook_fixed_true;
    vm.test_watcher_fire_hook      = fire_hook_count;

    /* Single dirty-eval pass — all 3 see nil→true rising edge. */
    vm.watcher_dirty_count = 1u;
    watcher_eval_dirty(&vm);

    /* All 3 must have fired. */
    UASSERT_EQ(g_fire_count, 3);

    urbi_watcher_unregister_internal(&vm, w1);
    urbi_watcher_unregister_internal(&vm, w2);
    urbi_watcher_unregister_internal(&vm, w3);
    uvm_destroy(&vm);
}

/* === Suite entry point === */

void
test_watcher_dirty_suite(void)
{
    printf("test_watcher_dirty\n");
    /* T33 cases */
    utest_run("watcher_install_sets_bit6",
              watcher_install_sets_bit6);
    utest_run("watcher_overlap_keeps_bit6_until_last",
              watcher_overlap_keeps_bit6_until_last);
    utest_run("watcher_install_inserts_into_tag_member_list",
              watcher_install_inserts_into_tag_member_list);
    utest_run("observer_dirty_bumps_counter",
              observer_dirty_bumps_counter);
    utest_run("watcher_active_count_tracks_install_unregister",
              watcher_active_count_tracks_install_unregister);
    utest_run("active_list_is_tail_inserted",
              active_list_is_tail_inserted);
    utest_run("watcher_install_readset_overflow_returns_null",
              watcher_install_readset_overflow_returns_null);
    /* T34 cases */
    utest_run("watcher_eval_dirty_skips_when_count_zero",
              watcher_eval_dirty_skips_when_count_zero);
    utest_run("watcher_eval_dirty_resets_count_to_zero",
              watcher_eval_dirty_resets_count_to_zero);
    utest_run("watcher_eval_at_edge_only_fires_on_false_to_true",
              watcher_eval_at_edge_only_fires_on_false_to_true);
    utest_run("watcher_eval_whenever_level_fires_each_dirty_pass",
              watcher_eval_whenever_level_fires_each_dirty_pass);
    utest_run("watcher_eval_skips_pending_unregister",
              watcher_eval_skips_pending_unregister);
    utest_run("watcher_install_seeds_last_value_cache",
              watcher_install_seeds_last_value_cache);
    /* (case 14 watcher_scratch_frame_allocated_at_init removed in Wave 1
     * v0.5.3-layout per WATCH-022 — defended a now-dead allocation.) */
    /* T35 cases */
    utest_run("pending_onleave_push_sets_flag_and_unlinks_from_active",
              pending_onleave_push_sets_flag_and_unlinks_from_active);
    utest_run("pending_onleave_drain_walks_until_empty",
              pending_onleave_drain_walks_until_empty);
    utest_run("pending_onleave_drain_invokes_hook_when_onleave_set",
              pending_onleave_drain_invokes_hook_when_onleave_set);
    utest_run("pending_onleave_drain_skips_null_onleave",
              pending_onleave_drain_skips_null_onleave);
    utest_run("pending_onleave_drain_ordering_FIFO",
              pending_onleave_drain_ordering_FIFO);
    utest_run("tag_stop_pushes_watchers_to_onleave_queue",
              tag_stop_pushes_watchers_to_onleave_queue);
    /* T36 cases */
    utest_run("watcher_root_walker_visits_active_watchers",
              watcher_root_walker_visits_active_watchers);
    utest_run("watcher_root_walker_visits_pending_onleave",
              watcher_root_walker_visits_pending_onleave);
    utest_run("watcher_root_walker_skips_null_closures",
              watcher_root_walker_skips_null_closures);
    utest_run("watcher_root_provider_count_is_6_after_init",
              watcher_root_provider_count_is_6_after_init);
    utest_run("spawn_body_coroutine_relocated_still_works",
              spawn_body_coroutine_relocated_still_works);
    /* §9.2 gap-fill: all-watchers walk in one eval pass */
    utest_run("eval_pass_walks_all_watchers",
              eval_pass_walks_all_watchers);
}
