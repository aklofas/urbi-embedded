/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: urbi_get_determinism_checksum — two runs produce identical
 * checksums when given the same namespace state (row 12 §8.2 determinism
 * acceptance tests).
 *
 * Substantive tests require URBI_DEBUG (checksum function absent otherwise).
 * A single smoke test runs in both modes to confirm compilation.
 *
 * make test       — default build: 1 case (smoke).
 * make test-debug — URBI_DEBUG=1: 1 smoke + 3 determinism cases.
 *
 * Note: this file tests cross-VM-within-one-process determinism using
 * UVAL_INT bindings only.  UVAL_STR values use interned pointer addresses
 * which are stable within a single VM but may differ between two VMs in the
 * same process.  Cross-process determinism is verified by the CI determinism
 * gate (T42), which runs the same program twice in separate processes and
 * compares checksums through a file/pipe mechanism. */

#include "utest.h"
#include "urbi/urbi.h"
#include "uvm.h"

#include <string.h>

#define UTEST(name) static void name(void)

/* ---- Smoke test: compiles in both debug and release modes ---- */

UTEST(two_runs_smoke)
{
    UASSERT(1);
}

/* ---- URBI_DEBUG-only tests ---- */

#ifdef URBI_DEBUG

#include "realm/urealm.h"
#include "umodule.h"  /* UValue, UVAL_INT */

/* Helper: bind one integer and return checksum. */
static uint64_t
checksum_with_int_binding(const char *name, int64_t value)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_global(&vm);
    UValue v;
    v.kind = UVAL_INT;
    v.v.i  = value;
    /* Intern the key before passing to unamespace_set, which expects an
     * interned pointer for == comparison during lookup. */
    const char *iname = ustr_intern(&vm, name, strlen(name));
    UASSERT(iname != NULL);
    (void)unamespace_set(&vm, r->bindings, iname, v);

    uint64_t h = urbi_get_determinism_checksum(&vm);
    uvm_destroy(&vm);
    return h;
}

/* Case: two VMs that receive the same integer binding produce the same checksum.
 * Uses the cross-VM-stable UVAL_INT path (no pointer mixing). */
UTEST(two_runs_same_int_binding_match)
{
    uint64_t h1 = checksum_with_int_binding("x", 42LL);
    uint64_t h2 = checksum_with_int_binding("x", 42LL);
    UASSERT(h1 == h2);
}

/* Case: two VMs with different integer values produce different checksums. */
UTEST(two_runs_different_int_binding_differ)
{
    uint64_t h1 = checksum_with_int_binding("x", 42LL);
    uint64_t h2 = checksum_with_int_binding("x", 43LL);
    UASSERT(h1 != h2);
}

/* Case: two fresh VMs with no bindings produce the same checksum.
 * Validates the FNV-1a seed + gc/intern counter mixing is cross-VM stable
 * for an empty, untouched VM. */
UTEST(two_runs_empty_vms_match)
{
    UVM vm1, vm2;
    uvm_init(&vm1, NULL, NULL);
    uvm_init(&vm2, NULL, NULL);

    uint64_t h1 = urbi_get_determinism_checksum(&vm1);
    uint64_t h2 = urbi_get_determinism_checksum(&vm2);
    UASSERT(h1 == h2);

    uvm_destroy(&vm1);
    uvm_destroy(&vm2);
}

#endif /* URBI_DEBUG */

/* ---- Suite registration ---- */

void test_determinism_two_runs_suite(void)
{
    utest_run("two_runs_smoke", two_runs_smoke);
#ifdef URBI_DEBUG
    utest_run("two_runs_same_int_binding_match",
              two_runs_same_int_binding_match);
    utest_run("two_runs_different_int_binding_differ",
              two_runs_different_int_binding_differ);
    utest_run("two_runs_empty_vms_match",
              two_runs_empty_vms_match);
#endif
}
