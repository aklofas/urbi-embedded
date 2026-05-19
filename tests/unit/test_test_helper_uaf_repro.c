/* SPDX-License-Identifier: BSD-3-Clause */
/* Reproducer for the test-helper UAF documented in design-risks
 * "v0.7.x — test-helper utest_e2e_compile_and_run UAFs on still-live
 * nested protos".
 *
 * The unsafe shape: utest_e2e_compile_and_run (bare, not _with_module)
 * compiles a chunk that installs a closure into a realm global and then
 * calls umodule_destroy.  If that closure still holds a UProto pointer
 * into the freed module's nested[] array, any subsequent dispatch through
 * it is a heap-use-after-free.
 *
 * Pre-v0.7.3 + v0.8.0 refcount mechanisms: ASan heap-use-after-free in
 * dispatch_loop_until_yield at the UProto pointer dereference.
 *
 * Post-v0.7.3 (UProto refcount + rescue) + v0.8.0 (UModule refcount +
 * deferred destroy): outcome TBD — this test classifies it.
 * Phase 4 of v0.8.1-uproto-root branches on the outcome. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "chunk/umodule.h"
#include "value/uarena.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* Shared drain helper (matches pattern in test_athandler_class_method_fatal.c). */
static UStepResult
drain_to_quiescent_uaf(UVM *vm)
{
    UStepResult r = URBI_STEP_QUIESCENT;
    int i;
    for (i = 0; i < 200; i++) {
        r = urbi_step(vm, 500, NULL);
        if (r == URBI_STEP_QUIESCENT || r == URBI_STEP_WAKE_AT ||
            r == URBI_STEP_FATAL) {
            return r;
        }
    }
    return r;
}

/* === Core repro ============================================================
 *
 * Uses utest_e2e_compile_and_run (bare, NOT _with_module).  The helper
 * destroys the install module after urbi_run_chunk returns.  Realm.c.press
 * holds a UClosure whose UProto pointer lives in the freed module's
 * nested[] array.  Firing the at-handler dispatches through that pointer.
 *
 * Pre-fix: ASan heap-use-after-free in dispatch_loop_until_yield.
 * Post-fix: clean execution (UProto refcount rescue keeps proto alive). */
UTEST(class_method_at_handler_after_module_destroy)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t ev = urbi_event_register(&vm, r, "ev", NULL, NULL);
    UASSERT(ev != URBI_EVENT_ID_INVALID);

    /* Bare compile_and_run: module is destroyed after urbi_run_chunk.
     * The at-handler's body closure (Realm.c.press) stays reachable via
     * the realm global but its backing UProto was in the now-freed module. */
    int rc = utest_e2e_compile_and_run(&vm,
        "class C { var press = function () { 1 } };"
        "Realm.c = C.new();"
        "at (ev?) Realm.c.press()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Fire the at-handler.  Pre-fix: ASan UAF here.
     * Post-fix: clean execution. */
    urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
    UStepResult step = drain_to_quiescent_uaf(&vm);

    /* The step must not be fatal.  A fatal result indicates either the
     * original UAF (now a controlled crash/sanitizer hit) or a regression
     * in the at-body dispatch path. */
    int fatal = (step == URBI_STEP_FATAL);
    UASSERT(!fatal);

    if (!fatal) {
        urbi_vm_destroy(&vm);
    }
    /* If fatal, leak vm — same convention as test_athandler_class_method_fatal.c
     * so the subsequent test cases still run cleanly. */
}

/* === Variant: side-effect probe ==========================================
 *
 * Same bare-module-destroy shape, but the method body increments a realm
 * counter so we can distinguish "fatal/skipped" from "ran successfully".
 * Pre-fix: body never runs (counter stays 0, or ASan terminates first).
 * Post-fix: counter == 1 after one event fire. */
UTEST(class_method_at_handler_side_effect_after_module_destroy)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t ev = urbi_event_register(&vm, r, "ev", NULL, NULL);
    UASSERT(ev != URBI_EVENT_ID_INVALID);

    /* Seed realm.n = 0. */
    UValue zero;
    zero.kind  = UVAL_INT;
    zero.v.i   = 0;
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "n", 1, zero));

    /* Bare compile_and_run destroys the module after urbi_run_chunk. */
    int rc = utest_e2e_compile_and_run(&vm,
        "class C {"
        "  var press = function () { Realm.n = Realm.n + 1 }"
        "};"
        "Realm.c = C.new();"
        "at (ev?) Realm.c.press()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
    UStepResult step = drain_to_quiescent_uaf(&vm);
    int fatal = (step == URBI_STEP_FATAL);
    UASSERT(!fatal);

    if (!fatal) {
        UValue n;
        n.kind = UVAL_INT;
        n.v.i  = -1;
        UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "n", 1, &n));
        UASSERT_EQ((int)UVAL_INT, (int)n.kind);
        UASSERT_EQ(1LL, n.v.i);

        urbi_vm_destroy(&vm);
    }
}

/* === Suite entry. ========================================================= */
void
test_test_helper_uaf_repro_suite(void)
{
    utest_run("test_helper_uaf_repro: at-handler fires after bare module_destroy",
              class_method_at_handler_after_module_destroy);
    utest_run("test_helper_uaf_repro: side-effect counter probe after bare module_destroy",
              class_method_at_handler_side_effect_after_module_destroy);
}
