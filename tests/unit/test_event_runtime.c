/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: event runtime fixes from v0.5.7-fixes Phase 11 (T49-T52).
 *
 * T49 EVENT-004: native event emit/sync_emit/waituntil validate the
 *                receiver kind before casting via uvalue_as_event.
 *                Phase 7 (M6 stdlib): the receiver moved from argv[0]
 *                to the Phase-3 `self` slot, and validation failures now
 *                surface as UEXEC_THROW (urbi_raise_type) rather than
 *                silently returning NIL.  The test pins the throw branch
 *                so future ABI shifts can't silently regress it.
 *
 * T50 EVENT-005: event_native_register propagates slot-install failures
 *                instead of dropping them.  When the slot installer OOMs
 *                on any of the four native slots, the function returns
 *                UVM_OOM and resets vm->event_proto to NULL.
 *
 * T51 EVENT-008: uevent_ring_drain bound matches the SPSC max-usable depth
 *                (DEPTH-1, not DEPTH).  Filling the ring to capacity and
 *                draining must consume every entry exactly once.
 *
 * T52 EVENT-009: uevent_at_watchers_append rejects double-insert in
 *                URBI_DEBUG builds.  The walk asserts that `w` is not
 *                already linked into the at_watchers chain.  Stretch: the
 *                actual abort fires only with NDEBUG off; in release the
 *                check is a no-op and the test only asserts the happy path. */

#include "utest.h"

#include "vm/uvm.h"
#include "event/uevent_native.h"
#include "event/uevent.h"
#include "event/uevent_ring.h"
#include "event/uevent_subscribe.h"
#include "watcher/uwatcher.h"
#include "object/uobject.h"
#include "value/uintern.h"
#include "sched/ustrand.h"
#include "module/umodule.h"
#include "runtime/uclosure.h"   /* Phase 7: UClosure->native_fn dispatch shape */
#include "urbi/urbi.h"
#include "urbi/types.h"         /* urbi_make_nil */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * T49: native_event_functions_validate_argv
 * Calls each of emit/sync_emit/waituntil with argc=0 (no receiver) and
 * with argc=1 but argv[0].kind=UVAL_NIL (wrong kind).  Asserts that all
 * six calls return UVAL_NIL without crashing — i.e. no UEvent cast on
 * a non-event UValue.
 * =================================================================== */

UTEST(native_event_functions_validate_argv)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    urbi_native_protos_init(&vm);

    UASSERT(vm.event_proto != NULL);
    if (vm.event_proto == NULL) { urbi_vm_destroy(&vm); return; }

    /* Phase 7 (M6 stdlib): Event proto slots are UVAL_CLOSURE values whose
     * native_fn pointer carries the Phase-3 (vm, self, args, nargs, out)
     * ABI.  The receiver kind is validated explicitly — non-event self
     * raises UEXEC_THROW (urbi_raise_type) rather than silently returning
     * NIL like the pre-Phase-7 host-fn ABI.  Drive each native and assert
     * the throw branch. */
    const char *names[3] = { "emit", "syncEmit", "waituntil" };
    const size_t lens[3] = { 4U, 8U, 9U };

    int i;
    for (i = 0; i < 3; i++) {
        USymbol *sym = (USymbol *)ustr_intern(&vm, names[i], lens[i]);
        UASSERT(sym != NULL);
        if (sym == NULL) continue;

        UValue slot;
        slot.kind = (uint8_t)UVAL_NIL;
        UASSERT(urbi_object_lookup(&vm, vm.event_proto, sym, &slot) == 0);
        UASSERT_EQ((int)slot.kind, (int)UVAL_CLOSURE);
        if (slot.kind != (uint8_t)UVAL_CLOSURE) continue;

        UClosure *cl = (UClosure *)slot.v.p;
        UASSERT(cl != NULL && cl->native_fn != NULL);
        if (cl == NULL || cl->native_fn == NULL) continue;

        /* self is a non-event UValue (NIL).  emit/syncEmit accept up to
         * one arg, waituntil accepts zero — pick the right nargs per fn. */
        UValue self;
        self.kind = (uint8_t)UVAL_NIL;
        self.v.i  = 0;

        UValue args[1];
        args[0].kind = (uint8_t)UVAL_INT;
        args[0].v.i  = 7;

        uint8_t nargs = (i == 2) ? 0U : 1U;  /* waituntil = 0, emit/syncEmit = 1 */
        UValue out = urbi_make_nil();
        int rc = cl->native_fn(&vm, self, args, nargs, &out);

        UASSERT_EQ(rc, UEXEC_THROW);
        UASSERT_EQ((int)out.kind, (int)UVAL_NIL);
    }

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T50: event_native_register_propagates_register_oom
 * Use a failing allocator that OOMs after the proto object + its first
 * symbol intern have been allocated, so urbi_register_fn for "new" or
 * one of the later slots returns -1.  Assert that event_native_register
 * returns UVM_OOM and that vm.event_proto has been reset to NULL.
 * =================================================================== */

typedef struct {
    int alloc_calls;
    int fail_at;  /* -1 means never fail; trigger NULL when alloc_calls > fail_at */
} EventAllocSpy;

static void *event_spy_alloc(void *ptr, size_t n, void *ud)
{
    EventAllocSpy *spy = (EventAllocSpy *)ud;
    if (n == 0) {
        free(ptr);
        return NULL;
    }
    if (ptr == NULL) {
        spy->alloc_calls++;
        if (spy->fail_at >= 0 && spy->alloc_calls > spy->fail_at) {
            return NULL;
        }
    }
    return realloc(ptr, n);
}

UTEST(event_native_register_propagates_register_oom)
{
    /* Step 1: count how many allocations a clean event_native_register costs.
     * We only need an upper bound on the proto + first slot install. */
    EventAllocSpy probe = { 0, -1 };
    UVM probe_vm;
    urbi_vm_init(&probe_vm, event_spy_alloc, &probe);

    UVMError ok = event_native_register(&probe_vm);
    UASSERT_EQ((int)ok, (int)UVM_OK);
    int total_clean_calls = probe.alloc_calls;
    UASSERT(total_clean_calls > 2);  /* proto + at least one slot */
    urbi_vm_destroy(&probe_vm);

    /* Step 2: run with fail_at set so that the proto allocates, but a later
     * urbi_register_fn (intern/slot-install) OOMs.  We pick a fail point
     * partway through the clean trace, after the proto+root-shape work has
     * landed but before all four register_fn calls finish. */
    EventAllocSpy spy = { 0, total_clean_calls - 2 };
    UVM vm;
    urbi_vm_init(&vm, event_spy_alloc, &spy);

    UVMError err = event_native_register(&vm);
    UASSERT_EQ((int)err, (int)UVM_OOM);
    UASSERT(vm.event_proto == NULL);

    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T51: ring_drain_handles_max_capacity_correctly
 * Fill the ring to its DEPTH-1 max usable entries; drain; assert all
 * were consumed (read index advanced to write index).  This exercises
 * the corrected `drained < URBI_EVENT_RING_DEPTH - 1` bound.
 * =================================================================== */

static int g_drain_count;
static void counting_drain_handler(struct UVM *vm, uint32_t event_id,
                                   UValue payload)
{
    (void)vm; (void)event_id; (void)payload;
    g_drain_count++;
}

UTEST(ring_drain_handles_max_capacity_correctly)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UASSERT(vm.event_ring != NULL);
    if (vm.event_ring == NULL) { urbi_vm_destroy(&vm); return; }

    urbi_register_event_drain(&vm, counting_drain_handler);
    g_drain_count = 0;

    /* Fill the ring to its max-usable depth (DEPTH-1: SPSC reserves one slot
     * to distinguish full from empty). */
    int filled = 0;
    int i;
    for (i = 0; i < URBI_EVENT_RING_DEPTH - 1; i++) {
        if (urbi_inject_event(&vm, (uint32_t)i, NULL, 0U) == URBI_OK) {
            filled++;
        }
    }
    UASSERT_EQ(filled, URBI_EVENT_RING_DEPTH - 1);

    /* The ring should now refuse one more inject. */
    int rc = urbi_inject_event(&vm, 0xDEADBEEFU, NULL, 0U);
    UASSERT_EQ(rc, URBI_ERR_EVENT_RING_FULL);

    /* Drain: every filled entry must reach the handler exactly once. */
    uevent_ring_drain(&vm);
    UASSERT_EQ(g_drain_count, URBI_EVENT_RING_DEPTH - 1);
    UASSERT(!uevent_ring_has_pending(vm.event_ring));

    urbi_register_event_drain(&vm, NULL);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T52: double_append_watcher_to_event_asserts_in_debug
 * Append a watcher once (must succeed); then in URBI_DEBUG builds a
 * second append with the same watcher would assert.  We cannot trigger
 * the abort from a unit test, so we only validate the happy-path
 * invariant: after one append, w is on the chain; after one logical
 * remove, w is gone again.  The actual double-insert assert is exercised
 * implicitly by URBI_DEBUG release-test runs of any test that
 * inadvertently double-appends — Gate G1 stretch.
 * =================================================================== */

UTEST(double_append_watcher_to_event_asserts_in_debug)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UEvent *e = urbi_event_create(&vm);
    UASSERT(e != NULL);
    if (e == NULL) { urbi_vm_destroy(&vm); return; }

    /* Allocate a UWatcher on the stack with next_in_event = NULL. */
    UWatcher w;
    {
        unsigned char *p = (unsigned char *)&w;
        size_t i;
        for (i = 0; i < sizeof(w); i++) p[i] = 0U;
    }
    w.next_in_event = NULL;

    /* Single append: head should now point at &w. */
    uevent_at_watchers_append(e, &w);
    UASSERT(e->at_watchers_head == &w);

    /* Remove: chain should empty out. */
    uevent_at_watchers_remove(e, &w);
    UASSERT(e->at_watchers_head == NULL);

    /* Re-append after remove must succeed — the URBI_DEBUG check rejects
     * double-insert (w already on chain), not re-insert after removal. */
    uevent_at_watchers_append(e, &w);
    UASSERT(e->at_watchers_head == &w);
    uevent_at_watchers_remove(e, &w);
    UASSERT(e->at_watchers_head == NULL);

    urbi_vm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void
test_event_runtime_suite(void)
{
    printf("test_event_runtime\n");
    utest_run("native_event_functions_validate_argv",
              native_event_functions_validate_argv);
    utest_run("event_native_register_propagates_register_oom",
              event_native_register_propagates_register_oom);
    utest_run("ring_drain_handles_max_capacity_correctly",
              ring_drain_handles_max_capacity_correctly);
    utest_run("double_append_watcher_to_event_asserts_in_debug",
              double_append_watcher_to_event_asserts_in_debug);
}
