/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: UWatcher pool init, alloc, free, exhaustion, high-water tracking.
 * Row 11 / T32. */

#include "utest.h"
#include "vm/uvm.h"
#include "watcher/uwatcher.h"
#include "twatcher_install_helper.h"
#include "gc/ugc.h"            /* UTYPE_WATCHER */
#include "gc/ugc_incremental.h" /* UGC_IS_FIXED */

#include <stdlib.h>   /* realloc / free — test-side only; NOT in src/ */
#include <stddef.h>

#define UTEST(name) static void name(void)

/* === Test cases === */

/* 1. watcher_pool_init_threads_freelist:
 *    After urbi_vm_init the pool slab is non-NULL, freelist head equals slab[0],
 *    active_watchers_head is NULL, in_use is 0, and the freelist has exactly
 *    URBI_WATCHER_POOL_SIZE entries. */
UTEST(watcher_pool_init_threads_freelist)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Pool must be allocated. */
    UASSERT(vm.watchers->pool_base != NULL);
    UASSERT(vm.watchers->pool_freelist == &vm.watchers->pool_base[0]);
    UASSERT(vm.active_watchers_head == NULL);
    UASSERT_EQ((int)vm.watchers->pool_in_use,    0);
    UASSERT_EQ((int)vm.watchers->pool_high_water, 0);

    /* Walk the freelist and count entries. */
    {
        UWatcher *cur = vm.watchers->pool_freelist;
        int count = 0;
        while (cur != NULL) {
            count++;
            cur = cur->next_active;
        }
        UASSERT_EQ(count, URBI_WATCHER_POOL_SIZE);
    }

    urbi_vm_destroy(&vm);
}

/* 2. watcher_record_size_is_within_budget:
 *    sizeof(UWatcher) must be <= 256 bytes on the default build
 *    (URBI_WATCHER_READSET_MAX=16 → spec §5.1 table shows ~208 B).
 *    Note: actual size depends on URBI_WATCHER_READSET_MAX at compile time. */
UTEST(watcher_record_size_is_within_budget)
{
    /* The spec §5.1 size budget table:
     *   READSET_MAX=4  (footprint): 112 B
     *   READSET_MAX=16 (default):   208 B  (fits within 256)
     *   READSET_MAX=64 (linux):     592 B
     * We assert <= 256 for the standard (non-linux) case; if READSET_MAX > 16
     * the caller deliberately opted in to a larger record. */
#if URBI_WATCHER_READSET_MAX <= 16
    UASSERT(sizeof(UWatcher) <= 256U);
#endif
    /* At minimum, the header fields must produce a non-trivial record. */
    UASSERT(sizeof(UWatcher) >= 64U);
}

/* 3. pool_alloc_returns_active_watcher:
 *    Install a watcher, verify field values, in_use counter, and head pointer.
 *    Then unregister and verify counter and head return to 0 / NULL. */
UTEST(pool_alloc_returns_active_watcher)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UWatcher *w = urbi_watcher_install_for_test(
        &vm,
        UWATCHER_AT,  /* mode */
        NULL,         /* owning_tag — NULL for this stub test */
        NULL,         /* condition */
        NULL,         /* body */
        NULL,         /* onleave */
        NULL,         /* read_set — T33 wires */
        0             /* read_set_count */
    );

    UASSERT(w != NULL);
    UASSERT_EQ((unsigned)w->type_tag, (unsigned)UTYPE_WATCHER);
    UASSERT(w->flags & URBI_WATCHER_ACTIVE);
    UASSERT_EQ((unsigned)w->mode, (unsigned)UWATCHER_AT);
    /* gc_byte must have UGC_IS_FIXED set. */
    UASSERT(w->gc_byte & UGC_IS_FIXED);
    /* in_use should be 1. */
    UASSERT_EQ((int)vm.watchers->pool_in_use, 1);
    /* active_watchers_head must point to this watcher. */
    UASSERT(vm.active_watchers_head == w);

    /* Unregister. */
    urbi_watcher_unregister_internal(&vm, w);

    UASSERT_EQ((int)vm.watchers->pool_in_use, 0);
    UASSERT(vm.active_watchers_head == NULL);

    urbi_vm_destroy(&vm);
}

/* 4. pool_exhaustion_returns_null:
 *    Install URBI_WATCHER_POOL_SIZE watchers — must all succeed.
 *    The (POOL_SIZE+1)th install must return NULL.
 *    Unregister all to clean up. */
UTEST(pool_exhaustion_returns_null)
{
    UVM vm;
    UWatcher *watchers[URBI_WATCHER_POOL_SIZE];
    int i;

    urbi_vm_init(&vm, NULL, NULL);

    for (i = 0; i < URBI_WATCHER_POOL_SIZE; i++) {
        watchers[i] = urbi_watcher_install_for_test(
            &vm, UWATCHER_AT, NULL, NULL, NULL, NULL, NULL, 0);
        UASSERT(watchers[i] != NULL);
    }

    /* Pool is exhausted — next install must fail. */
    {
        UWatcher *extra = urbi_watcher_install_for_test(
            &vm, UWATCHER_AT, NULL, NULL, NULL, NULL, NULL, 0);
        UASSERT(extra == NULL);
    }

    UASSERT_EQ((int)vm.watchers->pool_in_use, URBI_WATCHER_POOL_SIZE);

    /* Unregister all to clean up before destroy. */
    for (i = 0; i < URBI_WATCHER_POOL_SIZE; i++) {
        urbi_watcher_unregister_internal(&vm, watchers[i]);
    }

    UASSERT_EQ((int)vm.watchers->pool_in_use, 0);

    urbi_vm_destroy(&vm);
}

/* 5. pool_high_water_tracks_peak:
 *    Install 3 watchers, then unregister 2.
 *    high_water must still read 3 even after in_use drops to 1. */
UTEST(pool_high_water_tracks_peak)
{
    UVM vm;
    UWatcher *w[3];

    urbi_vm_init(&vm, NULL, NULL);

    w[0] = urbi_watcher_install_for_test(&vm, UWATCHER_AT, NULL, NULL, NULL, NULL, NULL, 0);
    w[1] = urbi_watcher_install_for_test(&vm, UWATCHER_AT, NULL, NULL, NULL, NULL, NULL, 0);
    w[2] = urbi_watcher_install_for_test(&vm, UWATCHER_AT, NULL, NULL, NULL, NULL, NULL, 0);

    UASSERT(w[0] != NULL && w[1] != NULL && w[2] != NULL);
    UASSERT_EQ((int)vm.watchers->pool_in_use,    3);
    UASSERT_EQ((int)vm.watchers->pool_high_water, 3);

    /* Unregister two. */
    urbi_watcher_unregister_internal(&vm, w[0]);
    urbi_watcher_unregister_internal(&vm, w[1]);

    UASSERT_EQ((int)vm.watchers->pool_in_use,    1);
    /* high_water must still be 3 — peak is not reduced by frees. */
    UASSERT_EQ((int)vm.watchers->pool_high_water, 3);

    /* Clean up. */
    urbi_watcher_unregister_internal(&vm, w[2]);

    urbi_vm_destroy(&vm);
}

/* 6. pool_destroy_zeroes_pending_onleave_head (WATCH-003):
 *    uwatcher_pool_destroy must explicitly NULL pending_onleave_head as part
 *    of its defensive zero pass.  Today the drain loop happens to NULL it via
 *    the *head = w->next_active assignment, but the defensive block at the
 *    function tail only zeroes watcher_pool_base / watcher_pool_freelist /
 *    active_watchers_head — leaving pending_onleave_head dependent on the
 *    drain loop's side effect.  This test locks the invariant so a future
 *    refactor of drain_watcher_list cannot leave it dangling.
 *
 *    We exercise the empty-pending case and the populated-pending case;
 *    in both, post-destroy pending_onleave_head must be NULL. */
UTEST(pool_destroy_zeroes_pending_onleave_head)
{
    /* Case A: empty pending list. */
    {
        UVM vm;
        urbi_vm_init(&vm, NULL, NULL);
        UASSERT(vm.pending_onleave_head == NULL);
        urbi_vm_destroy(&vm);
        /* Post-destroy invariant: pending_onleave_head is NULL. */
        UASSERT(vm.pending_onleave_head == NULL);
        UASSERT(vm.pending_onleave_tail == NULL);
    }

    /* Case B: populated pending list (one watcher pre-pushed onto the
     * pending_onleave queue, then destroy drains + zeroes). */
    {
        UVM vm;
        UWatcher *w;
        urbi_vm_init(&vm, NULL, NULL);

        w = urbi_watcher_install_for_test(
            &vm, UWATCHER_AT, NULL, NULL, NULL, NULL, NULL, 0);
        UASSERT(w != NULL);

        /* Move w from active_watchers_head to pending_onleave_head by hand —
         * mimics the urbi_tag_stop / urbi_watcher_pending_onleave_queue_push transition
         * without depending on the tag layer.  active_watchers_head must be
         * cleared so urbi_vm_destroy's drain doesn't double-process w. */
        UASSERT(vm.active_watchers_head == w);
        vm.active_watchers_head = NULL;
        w->next_active = NULL;
        vm.pending_onleave_head = w;
        vm.pending_onleave_tail = w;

        urbi_vm_destroy(&vm);

        /* Post-destroy invariant: both head and tail are NULL. */
        UASSERT(vm.pending_onleave_head == NULL);
        UASSERT(vm.pending_onleave_tail == NULL);
    }
}

/* === Suite entry point === */

void
test_watcher_pool_suite(void)
{
    printf("test_watcher_pool\n");
    utest_run("watcher_pool_init_threads_freelist",
              watcher_pool_init_threads_freelist);
    utest_run("watcher_record_size_is_within_budget",
              watcher_record_size_is_within_budget);
    utest_run("pool_alloc_returns_active_watcher",
              pool_alloc_returns_active_watcher);
    utest_run("pool_exhaustion_returns_null",
              pool_exhaustion_returns_null);
    utest_run("pool_high_water_tracks_peak",
              pool_high_water_tracks_peak);
    utest_run("pool_destroy_zeroes_pending_onleave_head",
              pool_destroy_zeroes_pending_onleave_head);
}
