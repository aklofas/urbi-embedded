/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_vm_tag_scope.c — characterization pins for the
 * OP_PUSH_TAG / OP_POP_TAG dispatch arms (v0.10.15-vm-decomp-2, W1 stage 1).
 *
 * These pin the OBSERVABLE contract of the two tag-scope opcodes — that a
 * `t: { ... }` scope enters, runs its body, exits cleanly (LIFO, leak-free
 * under ASan), and that sequential scopes reuse cleanup-stack slots without
 * corrupting cleanup_depth.  They are written BEFORE the stage-1 extraction
 * of the arm bodies into uvm_tag_scope.{c,h} and MUST pass identically before
 * AND after that extraction (it is behavior-preserving — the zero-delta gate).
 *
 * Note on the user-tag binding gap (v0.10.9-B): at the v0.10.14 baseline,
 * `t: { ... }` creates an ANONYMOUS scope and does not bind to the user tag t
 * (OP_PUSH_TAG ignores its A[3:0] nibble).  A characterization of "t.stop()
 * from inside the scope" is therefore NOT pinned here: with a real tag it
 * currently throws "tag.stop with no active scope" (the D3 outside-scope
 * path), which unwinds through urbi_unwind rather than tightly exercising the
 * push/pop arms this stage extracts.  That binding behavior — and its flip —
 * is covered by the W2 tests (test_tag_scope_binds_user_tag + the
 * scope_binds_user_tag.chk / tag_stop_with_finally.chk fixtures). */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* === Test 1: a bare `t: { <stmt> }` scope enters, runs the body, and exits;
 * the local mutated inside the scope is visible afterward (mirrors the
 * `var x = 0; t1: { x = 99 }; x` line of tests/chk/tag/scope.chk). */
UTEST(test_tag_scope_enters_and_exits)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UValue result = utest_e2e_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "var t = Tag.new(); var x = 0; t: { x = 7 }; x",
        &result);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)result.kind);
    UASSERT_EQ((int64_t)7, result.v.i);

    urbi_vm_destroy(&vm);
}

/* === Test 2: nested tag scopes push/pop in LIFO order without leaking.
 * The inner body runs; ASan over this suite catches any UTag / cleanup-entry
 * leak introduced by an extraction that drops the pop-time utag_destroy. */
UTEST(test_tag_scope_nested)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UValue result = utest_e2e_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "var a = Tag.new(); var b = Tag.new(); var n = 0; a: { b: { n = 1 } }; n",
        &result);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)result.kind);
    UASSERT_EQ((int64_t)1, result.v.i);

    urbi_vm_destroy(&vm);
}

/* === Test 3: two sequential scopes reuse the cleanup-stack slot freed by the
 * first scope's OP_POP_TAG; cleanup_depth returns to its prior level after
 * each pop so the second push lands in the same slot.  Pins that the pop path
 * correctly decrements depth (an extraction that drops urbi_sched_strand_cleanup_pop or
 * mis-orders it would leave depth high and break the second scope or leak). */
UTEST(test_tag_scope_sequential_reuse)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UValue result = utest_e2e_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "var t = Tag.new(); var x = 0; t: { x = 1 }; t: { x = x + 1 }; x",
        &result);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)result.kind);
    UASSERT_EQ((int64_t)2, result.v.i);

    urbi_vm_destroy(&vm);
}

/* === Test 4 (W2.1, v0.10.9-B): binding flip — once OP_PUSH_TAG honors the
 * tag_reg nibble, the strand running inside `t: { ... }` is a MEMBER of user
 * tag t, so t.stop() from inside is a clean in-scope tag-stop on t's OWN active
 * scope.  PRE-binding the scope was anonymous and unbound to t, so t had no
 * members and tag_stop_native took the D3 "no active scope" path, setting
 * vm->last_errmsg = "tag.stop with no active scope".  The discriminating pin is
 * therefore the ABSENCE of that D3 diagnostic post-binding.
 *
 * Note: the chunk strand still terminates via TAG_STOP here — tag-stop
 * "absorption" (resume execution AFTER the scope) is the separate, deferred
 * T29 concern (uunwind.c UCLEANUP_TAG_SCOPE is still the M3 stub), NOT part of
 * v0.10.9-B — so rc is intentionally not asserted.  Realm.hit stays 0 in both
 * regimes (the post-stop write never runs). */
UTEST(test_tag_scope_binds_user_tag)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "hit", 3,
                                              utest_e2e_make_int(0)));

    (void)utest_e2e_compile_and_run(&vm,
        "var t = Tag.new(); t: { t.stop(); Realm.hit = 1 }", NULL);

    /* Bound scope: the D3 outside-scope diagnostic must NOT appear.
     * (Pre-binding, vm.last_errmsg == "tag.stop with no active scope".) */
    UASSERT(strstr(vm.last_errmsg, "no active scope") == NULL);

    UValue v = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "hit", 3, &v));
    UASSERT_EQ((int)UVAL_INT, (int)v.kind);
    UASSERT_EQ((int64_t)0, v.v.i);

    urbi_vm_destroy(&vm);
}

/* === Suite entry. ==================================================== */
void
test_vm_tag_scope_suite(void)
{
    utest_run("vm_tag_scope: a bare t: { stmt } scope enters, runs, and exits",
              test_tag_scope_enters_and_exits);
    utest_run("vm_tag_scope: nested tag scopes push/pop LIFO without leaking",
              test_tag_scope_nested);
    utest_run("vm_tag_scope: sequential scopes reuse the popped cleanup slot",
              test_tag_scope_sequential_reuse);
    utest_run("vm_tag_scope: t: {...} binds the user tag; t.stop() inside unwinds",
              test_tag_scope_binds_user_tag);
}
