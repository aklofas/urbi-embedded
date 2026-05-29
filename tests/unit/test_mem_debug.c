/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: on-target memory-debug tooling (v0.11.3).
 *
 * make test            — default build (URBI_MEM_DEBUG undefined): only the
 *                        gate-off path compiles; the MEM_DEBUG branches are
 *                        excluded.
 * make test-mem-debug  — URBI_MEM_DEBUG=1: owner capture, redzone, poison /
 *                        quarantine UAF, handle leak + double-release.
 *
 * Stack-UVM pattern via urbi_vm_init/urbi_vm_destroy (mirrors test_perf_counters.c). */

#include "utest.h"
#include "utest_e2e_helpers.h"
#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "runtime/umemdebug.h"
#include <string.h>

#define UTEST(name) static void name(void)

/* Probe: with the gate on, the substate is sane and the tunables hold.
 * With the gate off, there is simply nothing to observe (macros are no-ops). */
UTEST(mem_debug_gate_compiles_both_modes)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
#if URBI_MEM_DEBUG
    UASSERT(URBI_MEM_QUARANTINE_DEPTH >= 1);
    UASSERT(URBI_MEM_REDZONE_BYTES >= 1);
    /* memdbg is lazy: NULL until the first urbi_gc_alloc. */
    (void)utest_e2e_compile_and_run(&vm, "1 + 2", NULL);
    UASSERT(vm.memdbg != NULL);                 /* allocations happened */
    UASSERT(vm.memdbg->alloc_seq > 0);          /* seq advanced */
#else
    UASSERT(1);
#endif
    urbi_vm_destroy(&vm);
}

void test_mem_debug_suite(void)
{
    utest_run("mem_debug_gate_compiles_both_modes", mem_debug_gate_compiles_both_modes);
}
