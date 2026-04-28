/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: watcher read-set capture, bit-6 lifecycle, observer_dirty.
 * Row 11 / T33. */

#include "utest.h"
#include "uvm.h"
#include "uwatcher.h"
#include "ugc.h"            /* UTYPE_OBJECT */
#include "ugc_capi.h"       /* urbi_gc_alloc */
#include "ugc_incremental.h" /* UGC_HAS_WATCHER_OBSERVER */
#include "utag.h"           /* utag_create / utag_destroy */

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

/* === Suite entry point === */

void
test_watcher_dirty_suite(void)
{
    printf("test_watcher_dirty\n");
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
}
