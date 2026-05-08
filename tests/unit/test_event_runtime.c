/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: event runtime fixes from v0.5.7-fixes Phase 11 (T49-T52).
 *
 * T49 EVENT-004: native event emit/sync_emit/waituntil validate argv[0]
 *                kind before casting via uvalue_as_event.  Misconfigured
 *                callers (wrong receiver kind) must get NIL back, not a
 *                garbage-cast crash inside the emit path.
 *
 * T50 EVENT-005: event_native_register propagates urbi_register_fn failures
 *                instead of dropping them.  When the slot installer OOMs
 *                on any of the four native slots, the function returns
 *                UVM_OOM and resets vm->event_proto to NULL. */

#include "utest.h"

#include "vm/uvm.h"
#include "event/uevent_native.h"
#include "event/uevent.h"
#include "object/uobject.h"
#include "value/uintern.h"
#include "sched/ustrand.h"
#include "module/umodule.h"
#include "urbi/urbi.h"

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

    /* Resolve the three native fns we want to drive. */
    const char *names[3] = { "emit", "syncEmit", "waituntil" };
    const size_t lens[3] = { 4U, 8U, 9U };

    UStrand caller;
    ustrand_init(&caller, &vm);

    int i;
    for (i = 0; i < 3; i++) {
        USymbol *sym = (USymbol *)ustr_intern(&vm, names[i], lens[i]);
        UASSERT(sym != NULL);
        if (sym == NULL) continue;

        UValue slot;
        slot.kind = (uint8_t)UVAL_NIL;
        UASSERT(urbi_object_lookup(&vm, vm.event_proto, sym, &slot) == 0);
        UASSERT_EQ((int)slot.kind, (int)UVAL_HOST_FN);
        if (slot.kind != (uint8_t)UVAL_HOST_FN) continue;

        UHostFn fn = (UHostFn)(uintptr_t)slot.v.p;

        /* Case A: argc == 0 — must return NIL without dereferencing argv[0]. */
        UValue zero_argv[1];
        zero_argv[0].kind = (uint8_t)UVAL_NIL;
        zero_argv[0].v.i  = 0;
        UValue r0 = fn(&caller, 0, zero_argv);
        UASSERT_EQ((int)r0.kind, (int)UVAL_NIL);

        /* Case B: argv[0] is wrong kind (UVAL_NIL, not UVAL_EVENT). */
        UValue argv[2];
        argv[0].kind = (uint8_t)UVAL_NIL;
        argv[0].v.i  = 0;
        argv[1].kind = (uint8_t)UVAL_INT;
        argv[1].v.i  = 7;
        UValue r1 = fn(&caller, 2, argv);
        UASSERT_EQ((int)r1.kind, (int)UVAL_NIL);
    }

    ustrand_destroy(&caller, &vm);
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

/* ===== Suite entry point ===== */

void
test_event_runtime_suite(void)
{
    printf("test_event_runtime\n");
    utest_run("native_event_functions_validate_argv",
              native_event_functions_validate_argv);
    utest_run("event_native_register_propagates_register_oom",
              event_native_register_propagates_register_oom);
}
