/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: urbi_get_determinism_checksum (URBI_DEBUG diagnostic).
 *
 * All substantive tests require URBI_DEBUG.  A single smoke test runs in
 * both modes to confirm the file compiles and links cleanly.
 *
 * make test       — default build: 1 case (smoke).
 * make test-debug — URBI_DEBUG=1: 1 smoke + 6 checksum sanity cases. */

#include "utest.h"
#include "urbi/urbi.h"
#include "vm/uvm.h"

#include <string.h>

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

#include "realm/urealm.h"
#include "module/umodule.h"      /* UValue, UValKind */
#include "object/uic.h"
#include "object/umodule_instance.h"

#include <stdlib.h>  /* malloc */

UTEST(determinism_checksum_returns_nonzero_on_empty_vm)
{
    /* FNV-1a seed is non-zero; any mix with gc/intern counters keeps it
     * non-zero.  The checksum must never be 0 on a freshly initialised VM. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    uint64_t h = urbi_get_determinism_checksum(&vm);
    UASSERT(h != 0);
    urbi_vm_destroy(&vm);
}

UTEST(determinism_checksum_is_stable_across_calls_on_quiescent_vm)
{
    /* Two successive calls on an untouched, quiescent VM must agree. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    uint64_t h1 = urbi_get_determinism_checksum(&vm);
    uint64_t h2 = urbi_get_determinism_checksum(&vm);
    UASSERT(h1 == h2);
    urbi_vm_destroy(&vm);
}

UTEST(determinism_checksum_differs_after_namespace_binding)
{
    /* Inserting a binding into the global realm must change the checksum. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    uint64_t h_before = urbi_get_determinism_checksum(&vm);

    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UValue v;
    v.kind  = UVAL_INT;
    v.v.i   = 42;
    /* Intern the key before passing to unamespace_set. */
    const char *ikey = ustr_intern(&vm, "x", 1);
    UASSERT(ikey != NULL);
    int rc = unamespace_set(&vm, r->bindings, ikey, v);
    UASSERT(rc == 0);  /* 0 = success from unamespace_set */

    uint64_t h_after = urbi_get_determinism_checksum(&vm);
    UASSERT(h_before != h_after);

    urbi_vm_destroy(&vm);
}

UTEST(determinism_checksum_two_identical_vms_match)
{
    /* Two VMs that receive the same single integer binding produce the same
     * checksum (integer values are mixed directly; no pointer dependence).
     * This test uses UVAL_INT (not UVAL_STR) because UVAL_STR mixes the
     * interned-pointer address, which is per-VM-stable but NOT cross-VM-stable —
     * two separate VMs binding identical strings would produce different
     * checksums.  T42's cross-process determinism test exercises the
     * cross-run-stable subset of state only. */
    UVM vm1, vm2;
    urbi_vm_init(&vm1, NULL, NULL);
    urbi_vm_init(&vm2, NULL, NULL);

    UValue v;
    v.kind  = UVAL_INT;
    v.v.i   = 99;

    URealm *r1 = urbi_realm_global(&vm1);
    URealm *r2 = urbi_realm_global(&vm2);
    UASSERT(r1 != NULL);
    UASSERT(r2 != NULL);

    /* Intern the key in each VM (intern table is per-VM; keys are stable within
     * their VM but may have different pointer addresses across VMs).  The
     * checksum walker visits only values, not keys, so cross-VM comparison is
     * valid for UVAL_INT payloads. */
    const char *ikey1 = ustr_intern(&vm1, "answer", 6);
    const char *ikey2 = ustr_intern(&vm2, "answer", 6);
    UASSERT(ikey1 != NULL);
    UASSERT(ikey2 != NULL);
    UASSERT(unamespace_set(&vm1, r1->bindings, ikey1, v) == 0);
    UASSERT(unamespace_set(&vm2, r2->bindings, ikey2, v) == 0);

    uint64_t h1 = urbi_get_determinism_checksum(&vm1);
    uint64_t h2 = urbi_get_determinism_checksum(&vm2);
    UASSERT(h1 == h2);

    urbi_vm_destroy(&vm1);
    urbi_vm_destroy(&vm2);
}

UTEST(determinism_checksum_includes_topology_gen)
{
    /* Per pre-M4 topology-generation spec §5: the determinism gate must
     * surface any divergence in shape-tree mutation ordering.  Bumping
     * topology_gen on an otherwise-quiescent VM must change the hash. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    uint64_t pre = urbi_get_determinism_checksum(&vm);
    vm.topology_gen += 5;
    uint64_t post = urbi_get_determinism_checksum(&vm);
    UASSERT(pre != post);
    urbi_vm_destroy(&vm);
}

UTEST(determinism_checksum_includes_next_object_id_and_lookup_id)
{
    /* Per pre-M4 prototype-chain spec §8.1: changes to next_object_id (and
     * lookup_id, by symmetry) must perturb the checksum.  Both counters are
     * mixed; flipping either produces a different hash. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    uint64_t pre = urbi_get_determinism_checksum(&vm);
    vm.next_object_id += 1;
    uint64_t post_oid = urbi_get_determinism_checksum(&vm);
    UASSERT(pre != post_oid);

    vm.lookup_id += 1;
    uint64_t post_lid = urbi_get_determinism_checksum(&vm);
    UASSERT(post_oid != post_lid);

    urbi_vm_destroy(&vm);
}

UTEST(determinism_checksum_folds_root_chunk_ic_state)
{
    /* T4 regression: entries[0] (root chunk) uses proto==NULL, so the old
     * ic_count derivation `(pi->proto != NULL) ? pi->proto->ic_count : 0`
     * always returned 0 — silently skipping root-chunk IC state from the
     * checksum.  The fix reads ic_count from mi->module->ic_count when i==0.
     *
     * Build a module with ic_count == 1 at the root level (no nested protos).
     * Create a UModuleInstance, verify the checksum changes after mutating
     * the root-chunk IC entry (entries[0].ic_table[0]). */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UModule m = {0};
    USymbol *xsym = (USymbol *)ustr_intern(&vm, "x", 1);
    UASSERT(xsym != NULL);

    /* Populate the module-level IC side table (root chunk). */
    m.ic_count = 1;
    m.ic_names = (USymbol **)malloc(1 * sizeof(USymbol *));
    UASSERT(m.ic_names != NULL);
    m.ic_names[0] = xsym;

    UModuleInstance *mi = urbi_module_instance_create(&vm, &m);
    UASSERT(mi != NULL);

    /* entries[0] must now have a real ic_table (T3 guarantee). */
    UProtoInstance *pi0 = &mi->proto_instances->entries[0];
    UASSERT(pi0->proto    == NULL);
    UASSERT(pi0->ic_table != NULL);

    /* Capture checksum before any IC mutation. */
    uint64_t h_before = urbi_get_determinism_checksum(&vm);

    /* Simulate a single IC fill: n=1, replace_cursor=1, topology_gen[0]=42. */
    pi0->ic_table[0].n              = 1;
    pi0->ic_table[0].replace_cursor = 1;
    pi0->ic_table[0].topology_gen[0] = 42;

    /* Checksum must change — root-chunk IC state is now folded in. */
    uint64_t h_after = urbi_get_determinism_checksum(&vm);
    UASSERT(h_before != h_after);

    urbi_module_instance_destroy(&vm, mi);
    umodule_destroy(&m);
    urbi_vm_destroy(&vm);
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
    utest_run("determinism_checksum_includes_topology_gen",
              determinism_checksum_includes_topology_gen);
    utest_run("determinism_checksum_includes_next_object_id_and_lookup_id",
              determinism_checksum_includes_next_object_id_and_lookup_id);
    utest_run("determinism_checksum_folds_root_chunk_ic_state",
              determinism_checksum_folds_root_chunk_ic_state);
#endif
}
