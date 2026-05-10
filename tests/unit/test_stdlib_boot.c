/* SPDX-License-Identifier: BSD-3-Clause */
/* test_stdlib_boot.c — M6 Phase 4 (Wave 2) stdlib boot integration smoke.
 *
 * Phase 4 wires the bake-tool blob (src/stdlib/urbi_stdlib_bytecode.gen.c)
 * into urbi_stdlib_boot via the deserialize + bind path.  At Phase 4
 * baseline the blob is empty (urbi_stdlib_bytecode_len == 0) so that
 * branch is dead; these tests validate:
 *
 *   1. vm_init_succeeds_with_empty_blob — boot path runs cleanly when
 *      the deserialize branch is skipped.
 *   2. wave1_realm_globals_reachable    — Wave 1 atom-proto + Object
 *      realm globals (Boolean / Nil / Void / Object) survive the
 *      Phase 4 wiring change and remain reachable post-boot.
 *   3. ic_name_resolution_post_boot     — names resolve through the
 *      realm-global lookup machinery after boot completes.  Phase 4
 *      satisfies this with the same realm-global reachability the
 *      previous test asserts; an end-to-end "evaluate `Boolean` as a
 *      chunk" check is unnecessary as the get_global path is the
 *      same lookup the OP_LOAD_REALM_GLOBAL site eventually walks.
 *
 * Phase 10 will replace the empty-blob assertion in Test 4 with a
 * fixed-size baseline once the order file is populated.  Test 5
 * exercises two-VM determinism (per-VM realm state independent). */

#include "utest.h"

#include "stdlib/stdlib_boot.h"     /* urbi_stdlib_bytecode, _len */
#include "vm/uvm.h"
#include "urbi/urbi.h"

#define UTEST(name) static void name(void)

/* === Test 1: vm_init succeeds and boot binds the stdlib blob ============== */

UTEST(vm_init_succeeds_and_binds_blob) {
    /* Phase 10: STDLIB_ORDER.txt is non-empty.  The bake produced a
     * positive-length blob; urbi_stdlib_boot deserializes it and binds
     * vm->stdlib_module on first realm creation. */
    UASSERT(urbi_stdlib_bytecode_len > 0u);

    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Force realm-create + populate, which calls urbi_stdlib_boot
     * and (Phase 10) runs the stdlib chunk so its top-level class
     * declarations install themselves as realm globals. */
    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);
    UASSERT_EQ((int)vm.stdlib_booted, 1);
    UASSERT(vm.stdlib_module != NULL);   /* non-empty blob → bound */

    urbi_vm_destroy(&vm);
}

/* === Test 2: Wave 1 realm globals (Boolean/Nil/Void/Object) reachable === */

UTEST(wave1_realm_globals_reachable) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UValue out = urbi_value_nil();
    int rc = urbi_realm_get_global(&vm, realm, "Boolean", 7, &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_OBJECT);

    out = urbi_value_nil();
    rc = urbi_realm_get_global(&vm, realm, "Nil", 3, &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_OBJECT);

    out = urbi_value_nil();
    rc = urbi_realm_get_global(&vm, realm, "Void", 4, &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_OBJECT);

    out = urbi_value_nil();
    rc = urbi_realm_get_global(&vm, realm, "Object", 6, &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_OBJECT);

    urbi_vm_destroy(&vm);
}

/* === Test 3: post-boot name lookup walks realm-global path ===
 *
 * Phase 4 satisfies this via the realm-global lookup helper; the same
 * machinery feeds OP_LOAD_REALM_GLOBAL at the bytecode level.  An
 * end-to-end "compile + run `Boolean`" smoke is redundant with Test 2
 * (same lookup).  We additionally assert that an unbound name returns
 * URBI_ERR_SLOT_NOT_FOUND, exercising the negative arm of the same
 * resolver. */

UTEST(ic_name_resolution_post_boot) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Positive: a Wave-1 stdlib name resolves. */
    UValue out = urbi_value_nil();
    int rc = urbi_realm_get_global(&vm, realm, "Object", 6, &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_OBJECT);

    /* Negative: an unbound name does not resolve, but the lookup path
     * itself is reachable (no fatal / OOM). */
    out = urbi_value_nil();
    rc = urbi_realm_get_global(&vm, realm,
                               "__no_such_global__", 18, &out);
    UASSERT_EQ(rc, URBI_ERR_SLOT_NOT_FOUND);

    urbi_vm_destroy(&vm);
}

/* === Test 4: blob is non-empty post-Phase-10 ================== */
/*
 * Phase 10 populated STDLIB_ORDER.txt with the .u overlay manifest.
 * The baked blob is now non-empty.  An exact-size pin would catch
 * unintended drift but is fragile under any minor lex/emit/serialize
 * change; the looser non-zero assertion is sufficient signal that
 * the bake produced output.  Wave-3 may tighten this to a hash. */

UTEST(blob_size_baseline) {
    UASSERT(urbi_stdlib_bytecode_len > 0u);
}

/* === Test 6: Phase 10 overlay realm globals reachable ==========
 *
 * The .u overlay shipped at Phase 10 declares Exception subclasses
 * (TypeError, KeyError, IndexError, etc.) as top-level
 * `class X : public Exception {}` decls.  Each desugars to a
 * realm-global write.  This test pins that the run-the-stdlib-chunk
 * step inside urbi_populate_realm_globals actually executed and the
 * subclass names resolve. */

UTEST(phase10_exception_subclasses_reachable) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    static const char *kSubclasses[] = {
        "TypeError", "ArityError", "LookupError", "KeyError",
        "IndexError", "RangeError", "DivByZero", "IOError",
        "CapacityError"
    };
    size_t i;
    for (i = 0; i < sizeof kSubclasses / sizeof kSubclasses[0]; i++) {
        UValue v = urbi_value_nil();
        const char *name = kSubclasses[i];
        size_t      nlen = 0;
        while (name[nlen]) nlen++;
        int rc = urbi_realm_get_global(&vm, realm, name, nlen, &v);
        UASSERT_EQ(rc, URBI_OK);
        UASSERT_EQ((int)v.kind, (int)UVAL_OBJECT);
    }
    urbi_vm_destroy(&vm);
}

/* === Test 5: two-VM determinism (per-VM realm state independent) ===
 *
 * Two parallel UVMs initialized in sequence MUST each see Wave-1 atom
 * protos as realm globals.  Catches regressions where the boot path
 * accidentally relies on process-global state that survives the
 * first urbi_vm_destroy and corrupts the second VM. */

UTEST(two_vm_determinism) {
    UVM vm_a;
    urbi_vm_init(&vm_a, NULL, NULL);
    struct URealm *realm_a = urbi_realm_global(&vm_a);
    UASSERT(realm_a != NULL);

    UVM vm_b;
    urbi_vm_init(&vm_b, NULL, NULL);
    struct URealm *realm_b = urbi_realm_global(&vm_b);
    UASSERT(realm_b != NULL);

    UValue out_a = urbi_value_nil();
    UValue out_b = urbi_value_nil();
    int rc_a = urbi_realm_get_global(&vm_a, realm_a, "Boolean", 7, &out_a);
    int rc_b = urbi_realm_get_global(&vm_b, realm_b, "Boolean", 7, &out_b);
    UASSERT_EQ(rc_a, URBI_OK);
    UASSERT_EQ(rc_b, URBI_OK);
    UASSERT_EQ((int)out_a.kind, (int)UVAL_OBJECT);
    UASSERT_EQ((int)out_b.kind, (int)UVAL_OBJECT);

    /* Per-VM atom proto pointers MUST differ — UObject singletons live
     * on the UVM, not in process-global state. */
    UASSERT(out_a.v.p != NULL);
    UASSERT(out_b.v.p != NULL);
    UASSERT(out_a.v.p != out_b.v.p);

    /* stdlib_booted toggled exactly once per VM. */
    UASSERT_EQ((int)vm_a.stdlib_booted, 1);
    UASSERT_EQ((int)vm_b.stdlib_booted, 1);

    urbi_vm_destroy(&vm_b);
    urbi_vm_destroy(&vm_a);
}

void
test_stdlib_boot_suite(void)
{
    utest_run("vm_init_succeeds_and_binds_blob",
              vm_init_succeeds_and_binds_blob);
    utest_run("wave1_realm_globals_reachable",
              wave1_realm_globals_reachable);
    utest_run("ic_name_resolution_post_boot",
              ic_name_resolution_post_boot);
    utest_run("blob_size_baseline",
              blob_size_baseline);
    utest_run("two_vm_determinism",
              two_vm_determinism);
    utest_run("phase10_exception_subclasses_reachable",
              phase10_exception_subclasses_reachable);
}
