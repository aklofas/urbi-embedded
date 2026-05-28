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
#include "runtime/uperf.h"

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

void test_perf_counters_suite(void)
{
    utest_run("perf_macro_compiles_both_modes", perf_macro_compiles_both_modes);
    utest_run("perf_gc_cycles_counted", perf_gc_cycles_counted);
}
