/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: VM-domain performance counters (v0.11.1).
 *
 * make test               — default build (URBI_PERF_COUNTERS undefined):
 *                           the macro is (void)0; only compile-out is checked.
 * make test-perf-counters — URBI_PERF_COUNTERS=1: increment / reset / counts.
 *
 * Stack-UVM pattern via urbi_vm_init/urbi_vm_destroy (mirrors test_trace.c). */

#include "utest.h"
#include "utest_e2e_helpers.h"
#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "runtime/uperf.h"
#include <string.h>

#define UTEST(name) static void name(void)

UTEST(perf_macro_compiles_both_modes)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    /* In both modes this must compile and not crash.  Check the macro's
     * delta rather than an absolute value: urbi_perf_reset is not wired into
     * urbi_vm_init until the next task, so the field is not zeroed here. */
#if URBI_PERF_COUNTERS
    {
        size_t before = vm.perf.opcodes;
        URBI_PERF_INC(&vm, opcodes);
        URBI_PERF_ADD(&vm, opcodes, 3);
        UASSERT_EQ(vm.perf.opcodes, before + 4u);
    }
#else
    URBI_PERF_INC(&vm, opcodes);
    URBI_PERF_ADD(&vm, opcodes, 3);
    UASSERT(1);  /* macro is (void)0; nothing to observe */
#endif
    urbi_vm_destroy(&vm);
}

UTEST(perf_gc_cycles_counted)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(vm.gc_cycles, (size_t)0);
    (void)utest_e2e_compile_and_run(&vm, "1 + 2", NULL);   /* allocate some cells */
    urbi_gc_force_full(&vm);                                /* one full cycle */
    UASSERT(vm.gc_cycles >= 1);
    urbi_vm_destroy(&vm);
}

UTEST(perf_opcodes_and_events_counted)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)utest_e2e_compile_and_run(&vm,
        "var e = Event.new(); at (e?) Realm.x = 1; e!(1)", NULL);
    utest_e2e_run_to_no_runnable(&vm);
#if URBI_PERF_COUNTERS
    UASSERT(vm.perf.opcodes > 0);
    UASSERT(vm.perf.event_emits >= 1);
    UASSERT(vm.perf.watcher_fires >= 1);
    UASSERT(vm.perf.ctx_switches >= 1);
#else
    UASSERT(1);
#endif
    urbi_vm_destroy(&vm);
}

#if defined(URBI_ENABLE_REPL)
UTEST(perf_debug_profile_populated)
{
    UVM vm; URealm *r; char out[1024];
    urbi_vm_init(&vm, NULL, NULL);
    r = urbi_realm_global(&vm);
    (void)urbi_repl_eval(&vm, r, "1 + 2 + 3", 9, out, sizeof out);
    (void)urbi_repl_eval(&vm, r, "Debug.profile()", 15, out, sizeof out);
    /* repl_eval returns the JSON as an escaped-quote String; match bare names.
     * The three locked keys stay present; the v0.9.1 "deferred" note is gone. */
    UASSERT(strstr(out, "counters") != NULL);
    UASSERT(strstr(out, "epoch") != NULL);
    UASSERT(strstr(out, "per_opcode") != NULL);     /* locked key still present */
    UASSERT(strstr(out, "profiling deferred") == NULL); /* note replaced */
    urbi_vm_destroy(&vm);
}

UTEST(perf_debug_gc_has_timing_fields)
{
    UVM vm; URealm *r; char out[512];
    urbi_vm_init(&vm, NULL, NULL);
    r = urbi_realm_global(&vm);
    (void)urbi_repl_eval(&vm, r, "Debug.gc()", 10, out, sizeof out);
    /* repl_eval returns the JSON as a String whose quotes are escaped in `out`,
     * so match bare field names (mirrors debug_gc_from_urbiscript_returns_*). */
    UASSERT(strstr(out, "cycles") != NULL);
    UASSERT(strstr(out, "slices") != NULL);
    UASSERT(strstr(out, "last_gc_us") != NULL);
    UASSERT(strstr(out, "total_gc_us") != NULL);
    urbi_vm_destroy(&vm);
}
#endif

void test_perf_counters_suite(void)
{
    utest_run("perf_macro_compiles_both_modes", perf_macro_compiles_both_modes);
    utest_run("perf_gc_cycles_counted", perf_gc_cycles_counted);
    utest_run("perf_opcodes_and_events_counted", perf_opcodes_and_events_counted);
#if defined(URBI_ENABLE_REPL)
    utest_run("perf_debug_profile_populated", perf_debug_profile_populated);
    utest_run("perf_debug_gc_has_timing_fields", perf_debug_gc_has_timing_fields);
#endif
}
