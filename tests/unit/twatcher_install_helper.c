/* SPDX-License-Identifier: BSD-3-Clause */
/* twatcher_install_helper: see twatcher_install_helper.h.
 *
 * Implementation lifted verbatim from the retired
 * src/watcher/uwatcher.c:urbi_watcher_install_internal (closes WATCH-023). */

#include "twatcher_install_helper.h"

#include "vm/uvm.h"
#include "watcher/uwatcher.h"   /* UWatcher, uwatcher_pool_alloc, UWATCHER_* */
#include "tag/utag.h"           /* UTag, member_watchers_head */
#include "gc/ugc.h"             /* UCell */
#include "gc/ugc_incremental.h" /* UGC_HAS_WATCHER_OBSERVER */
#include "urbi/urbi.h"          /* URBI_ASSERT_NOT_ISR */

#include <stddef.h>
#include <stdint.h>

struct UWatcher *
urbi_watcher_install_for_test(
    struct UVM       *vm,
    uint8_t           mode,
    struct UTag      *owning_tag,
    struct UClosure  *condition,
    struct UClosure  *body,
    struct UClosure  *onleave,
    struct UCell    **read_set,
    size_t            read_set_count)
{
    struct UWatcher *w;
    size_t           i;

    URBI_ASSERT_NOT_ISR(vm);

    /* Guard: overflow check before uwatcher_pool_alloc to avoid wasting a slot. */
    if (read_set_count > (size_t)URBI_WATCHER_READSET_MAX) return NULL;

    w = uwatcher_pool_alloc(vm);
    if (w == NULL) return NULL;

    w->mode       = mode;
    w->owning_tag = owning_tag;
    w->condition  = condition;
    w->body       = body;
    w->onleave    = onleave;
    w->read_set_count = (uint8_t)read_set_count;

    /* Read-set capture: populate cells[] and set bit-6 on each observed cell
     * per spec §5.3.  Caller may pass read_set == NULL when read_set_count == 0. */
    for (i = 0; i < read_set_count; i++) {
        read_set[i]->gc_byte |= UGC_HAS_WATCHER_OBSERVER;
        w->cells[i] = read_set[i];
    }

    /* Tail-insert into active_watchers_head: install order = eval order
     * (determinism gate relies on this invariant). */
    w->next_active = NULL;
    if (vm->active_watchers_head == NULL) {
        vm->active_watchers_head = w;
    } else {
        struct UWatcher *tail = vm->active_watchers_head;
        while (tail->next_active != NULL) tail = tail->next_active;
        tail->next_active = w;
    }

    /* Head-insert into owning tag's member_watchers_head per spec §5.4.
     * NULL-guard: tests may pass owning_tag == NULL. */
    if (owning_tag != NULL) {
        w->next_in_tag                   = owning_tag->member_watchers_head;
        owning_tag->member_watchers_head = w;
    } else {
        w->next_in_tag = NULL;
    }

    /* Track active count. */
    vm->watcher_active_count++;

    /* Seed last_value_cache with the current condition result per spec §6.3
     * ("at fires on transitions; not on initial truthy state").  The install-
     * time eval seeds the cache but does NOT fire the body: a subsequent dirty
     * pass that re-evaluates and finds new == old == truthy will not fire
     * (no rising edge for AT/AT_SYNC; WHENEVER fires on next dirty pass).
     *
     * Low-level bypass path: only seed via hook when set; otherwise nil.
     * Production watcher installs go through install_watcher_runtime which
     * calls run_closure_on_scratch_frame_with_result for real bytecode eval.
     * This helper is used by tests that may pass fake closure sentinels
     * without setting a condition hook. */
    if (w->condition != NULL && vm->test_hooks != NULL
            && vm->test_hooks->watcher_condition != NULL) {
        w->last_value_cache = vm->test_hooks->watcher_condition(vm, w);
    } else {
        UValue nil = {0};
        w->last_value_cache = nil;
    }

    return w;
}
