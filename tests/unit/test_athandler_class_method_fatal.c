/* SPDX-License-Identifier: BSD-3-Clause */
/* test_athandler_class_method_fatal — task #23 regression.
 *
 * Bug (open as of v0.7.2 pre-ship): an at-handler whose body invokes a
 * method on a class instance fatals the body strand on the first event
 * fire.  `urbi_step` returns URBI_STEP_FATAL with `vm->last_errmsg`
 * empty (no diagnostic).  On ESP32-S3-EYE hardware this manifests as
 * `esp_restart_noos` boot-loop; on host it surfaces as a fatal step
 * result from `urbi_step`.
 *
 * Adjacent shapes that DO work (verified by `wedge_at_body_with_list_get_field_read`
 * and the chained-at-handler tests):
 *   - at-body reading a class instance FIELD                     (works)
 *   - chunk-top SYNCHRONOUS call to a class instance method      (works)
 *   - at-body with a plain SETSLOT (no method call)              (works)
 *   - chained at-handlers without method calls                   (works)
 *
 * Failure mode here is specific to: at-body + method invocation on a
 * class instance ("at handler delegates to a method").  Documented at
 * `docs/urbi-embedded-design-risks.md` under "v0.7.x — at-body invoking
 * class instance method fatals body strand (OPEN)".  Pre-existing — not
 * introduced by S42, runs on top of S42-fixed code identically.
 *
 * Pass criteria for this file: each at-body method invocation reaches
 * quiescence cleanly (no URBI_STEP_FATAL) AND the method's observable
 * side-effect is visible after the event drain.  Until the fix lands,
 * all three of these will fail at the `URBI_STEP_FATAL` assertion. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "chunk/umodule.h"
#include "value/uarena.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* All three tests keep the install-chunk module alive across the event
 * drain.  Production embedders that ship pre-compiled bytecode never
 * destroy the module mid-run, and the documented task #23 symptom
 * (URBI_STEP_FATAL with empty errmsg — graceful, not a crash) shows up
 * on that path.  Destroying the module mid-run UAFs through a different
 * latent bug (`umodule_destroy` frees nested protos still reachable via
 * realm-rooted closures); the REPL avoids that via `urbi_steal_repl_protos`,
 * but `urbi_run_chunk` callers must hold the module themselves. */

/* Drain runnable queue to quiescence and surface any fatal step result. */
static UStepResult
drain_to_quiescent(UVM *vm)
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

/* === Test 1: minimal canonical repro from the task #23 record ============
 *
 *   class Cycle { var press = function () { 1 } };
 *   Realm.cycle = Cycle.new();
 *   at (ev?) Realm.cycle.press()
 *
 * One event fires the body; the body invokes press() on the Cycle
 * instance and discards the result.  Pre-fix: body strand fatals with
 * an empty errmsg.  Post-fix: drains cleanly. */
UTEST(at_body_calls_class_method_minimal)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t ev = urbi_event_register(&vm, r, "ev", NULL, NULL);
    UASSERT(ev != URBI_EVENT_ID_INVALID);

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "class Cycle { var press = function () { 1 } };"
        "Realm.cycle = Cycle.new();"
        "at (ev?) Realm.cycle.press()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
    UStepResult step = drain_to_quiescent(&vm);
    int fatal = (step == URBI_STEP_FATAL);
    UASSERT(!fatal);

    /* If the strand fataled, vm / module teardown asserts on the broken
     * state.  Leak intentionally so the failure surfaces cleanly and the
     * next test cases still run.  Pre-fix this leak is the cost of
     * exercising the bug; post-fix the !fatal branch runs cleanly. */
    if (!fatal) {
        uarena_destroy(&arena);
        umodule_destroy(&module, NULL);
        urbi_vm_destroy(&vm);
    }
}

/* === Test 2: side-effect probe — the method's body must actually run.
 *
 * Same shape as Test 1 but the method increments a Realm counter so we
 * can detect "fatal" vs "skipped" vs "ran successfully".  Pre-fix the
 * body fatals BEFORE the method body runs (counter stays 0); post-fix
 * counter == 1 after one event. */
UTEST(at_body_calls_class_method_side_effect)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t ev = urbi_event_register(&vm, r, "ev", NULL, NULL);
    UASSERT(ev != URBI_EVENT_ID_INVALID);

    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "n", 1,
                                               utest_e2e_make_int(0)));

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "class Cycle {"
        "  var press = function () { Realm.n = Realm.n + 1 }"
        "};"
        "Realm.cycle = Cycle.new();"
        "at (ev?) Realm.cycle.press()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
    UStepResult step = drain_to_quiescent(&vm);
    int fatal = (step == URBI_STEP_FATAL);
    UASSERT(!fatal);

    if (!fatal) {
        UValue n = utest_e2e_make_nil();
        UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "n", 1, &n));
        UASSERT_EQ((int)UVAL_INT, (int)n.kind);
        UASSERT_EQ(1LL, n.v.i);

        uarena_destroy(&arena);
        umodule_destroy(&module, NULL);
        urbi_vm_destroy(&vm);
    }
}

/* === Test 3: control — same shape but body reads a FIELD instead of
 * calling a METHOD.  Confirms the test infrastructure is correct: this
 * variant works (mirrors `wedge_at_body_with_list_get_field_read`).
 * If this fails, the at-body / class-instance path itself is broken
 * and task #23 has the wrong scope; if it passes while tests 1+2 fail,
 * the bug is METHOD-INVOCATION-specific within the at-body. */
UTEST(at_body_reads_class_field_control)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t ev = urbi_event_register(&vm, r, "ev", NULL, NULL);
    UASSERT(ev != URBI_EVENT_ID_INVALID);

    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "out", 3,
                                               utest_e2e_make_int(-1)));

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "class Cycle { var val = 42 };"
        "Realm.cycle = Cycle.new();"
        "at (ev?) Realm.out = Realm.cycle.val",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
    UStepResult step = drain_to_quiescent(&vm);
    UASSERT(step != URBI_STEP_FATAL);

    UValue out = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "out", 3, &out));
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(42LL, out.v.i);

    uarena_destroy(&arena);
    umodule_destroy(&module, NULL);
    urbi_vm_destroy(&vm);
}

/* Underlying mechanism (verified via diagnostic instrumentation 2026-05-16):
 * any SCRIPTED (UProto-backed) function call in the at-body triggers
 * OP_RET-from-non-top → pending_unwind=UEXEC_RETURN → urbi_unwind walker.
 * The body strand has a TAG_SCOPE cleanup entry at depth=1; the walker pops
 * it without absorbing the RETURN (TAG_SCOPE doesn't absorb RETURN), then
 * the loop exits at cleanup_depth=0 and falls through to `fatal:` which sets
 * fatal_status=UEXEC_RETURN.  Native-method calls (e.g. `List.new(...).get(0)`)
 * are NOT affected — they bypass OP_RET via the native_fn dispatch arm.  We
 * can't easily write a non-class-method version of the repro because
 * `Realm.f = function () {...}` at chunk-top hits task #22 first. */

/* === Test 4: task #13 / S43 — chunk-top `var` followed by at-body that
 * reads the var.  Pre-task-#23-fix this crashed at uunwind.h:37 with a
 * LoadProhibited on `s->module->constants` (body strand has
 * `s->module == NULL`; pre-fix `ustrand_consts_for_closure` did an
 * unconditional NULL deref).  Same root cause as task #23's
 * pop_call_frame issue, fixed by the same change (entry_closure
 * fallback chain).  Test confirms that fix covers the S43 shape. */
UTEST(s43_chunktop_var_then_at_body_reads_var)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t ev = urbi_event_register(&vm, r, "ev", NULL, NULL);
    UASSERT(ev != URBI_EVENT_ID_INVALID);

    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "out", 3,
                                               utest_e2e_make_int(-1)));

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "var c_red = 42;"
        "at (ev?) Realm.out = c_red",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
    UStepResult step = drain_to_quiescent(&vm);
    UASSERT(step != URBI_STEP_FATAL);

    UValue out = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "out", 3, &out));
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(42LL, out.v.i);

    uarena_destroy(&arena);
    umodule_destroy(&module, NULL);
    urbi_vm_destroy(&vm);
}

/* === Suite entry. ====================================================== */
void
test_athandler_class_method_fatal_suite(void)
{
    utest_run("athandler_class_method_fatal: minimal repro (task #23)",
              at_body_calls_class_method_minimal);
    utest_run("athandler_class_method_fatal: side-effect counter probe",
              at_body_calls_class_method_side_effect);
    utest_run("athandler_class_method_fatal: control — field read works",
              at_body_reads_class_field_control);
    utest_run("athandler_class_method_fatal: chunk-top var + at-body (S43)",
              s43_chunktop_var_then_at_body_reads_var);
}
