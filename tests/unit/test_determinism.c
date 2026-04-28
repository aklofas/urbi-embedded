/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: urbi_get_determinism_checksum (URBI_DEBUG diagnostic).
 *
 * All substantive tests require URBI_DEBUG.  A single smoke test runs in
 * both modes to confirm the file compiles and links cleanly.
 *
 * make test       — default build: 1 case (smoke).
 * make test-debug — URBI_DEBUG=1: 1 smoke + 4 checksum sanity cases. */

#include "utest.h"
#include "urbi.h"
#include "uvm.h"
#include <stddef.h>

#define UTEST(name) static void name(void)

/* ---- Smoke test: verify the file compiles in both debug and release modes --- */

UTEST(determinism_checksum_smoke)
{
    /* urbi_get_determinism_checksum is declared only in URBI_DEBUG builds;
     * this test simply confirms compilation succeeds in both modes. */
    UASSERT(1);
}

/* ---- URBI_DEBUG-only tests ------------------------------------------------- */

#ifdef URBI_DEBUG

#include "urealm.h"
#include "umodule.h"  /* UValue, UValKind */

UTEST(determinism_checksum_returns_nonzero_on_empty_vm)
{
    /* FNV-1a seed is non-zero; any mix with gc/intern counters keeps it
     * non-zero.  The checksum must never be 0 on a freshly initialised VM. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    uint64_t h = urbi_get_determinism_checksum(&vm);
    UASSERT(h != 0);
    uvm_destroy(&vm);
}

UTEST(determinism_checksum_is_stable_across_calls_on_quiescent_vm)
{
    /* Two successive calls on an untouched, quiescent VM must agree. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    uint64_t h1 = urbi_get_determinism_checksum(&vm);
    uint64_t h2 = urbi_get_determinism_checksum(&vm);
    UASSERT(h1 == h2);
    uvm_destroy(&vm);
}

UTEST(determinism_checksum_differs_after_namespace_binding)
{
    /* Inserting a binding into the global realm must change the checksum. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    uint64_t h_before = urbi_get_determinism_checksum(&vm);

    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UValue v;
    v.kind  = UVAL_INT;
    v.v.i   = 42;
    int rc = unamespace_set(&vm, r->bindings, "x", v);
    UASSERT(rc == 0);  /* 0 = success from unamespace_set */

    uint64_t h_after = urbi_get_determinism_checksum(&vm);
    UASSERT(h_before != h_after);

    uvm_destroy(&vm);
}

UTEST(determinism_checksum_two_identical_vms_match)
{
    /* Two VMs that receive the same single integer binding produce the same
     * checksum (integer values are mixed directly; no pointer dependence). */
    UVM vm1, vm2;
    uvm_init(&vm1, NULL, NULL);
    uvm_init(&vm2, NULL, NULL);

    UValue v;
    v.kind  = UVAL_INT;
    v.v.i   = 99;

    URealm *r1 = urbi_realm_global(&vm1);
    URealm *r2 = urbi_realm_global(&vm2);
    UASSERT(r1 != NULL);
    UASSERT(r2 != NULL);

    UASSERT(unamespace_set(&vm1, r1->bindings, "answer", v) == 0);
    UASSERT(unamespace_set(&vm2, r2->bindings, "answer", v) == 0);

    uint64_t h1 = urbi_get_determinism_checksum(&vm1);
    uint64_t h2 = urbi_get_determinism_checksum(&vm2);
    UASSERT(h1 == h2);

    uvm_destroy(&vm1);
    uvm_destroy(&vm2);
}

#endif /* URBI_DEBUG */

/* ---- Suite entry point ---------------------------------------------------- */

void test_determinism_suite(void)
{
    utest_run("determinism_checksum_smoke",
              determinism_checksum_smoke);
#ifdef URBI_DEBUG
    utest_run("determinism_checksum_returns_nonzero_on_empty_vm",
              determinism_checksum_returns_nonzero_on_empty_vm);
    utest_run("determinism_checksum_is_stable_across_calls_on_quiescent_vm",
              determinism_checksum_is_stable_across_calls_on_quiescent_vm);
    utest_run("determinism_checksum_differs_after_namespace_binding",
              determinism_checksum_differs_after_namespace_binding);
    utest_run("determinism_checksum_two_identical_vms_match",
              determinism_checksum_two_identical_vms_match);
#endif
}
