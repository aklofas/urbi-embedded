/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: event runtime fixes from v0.5.7-fixes Phase 11 (T49-T52).
 *
 * T49 EVENT-004: native event emit/sync_emit/waituntil validate argv[0]
 *                kind before casting via uvalue_as_event.  Misconfigured
 *                callers (wrong receiver kind) must get NIL back, not a
 *                garbage-cast crash inside the emit path. */

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

/* ===== Suite entry point ===== */

void
test_event_runtime_suite(void)
{
    printf("test_event_runtime\n");
    utest_run("native_event_functions_validate_argv",
              native_event_functions_validate_argv);
}
