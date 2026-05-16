/* SPDX-License-Identifier: BSD-3-Clause */
/* test_unwind_method_try_catch — regression for the third uunwind walker
 * NULL-deref discovered 2026-05-16 on ESP32-S3-EYE eye_demo hardware
 * during the urbiscript breadth-test stress run.
 *
 * Bug: `urbi_unwind` line 358 (TRY_FRAME catch absorption) and line 215
 * (run_cleanup_with_replace, finally/onleave bodies) both hardcode
 *
 *   s->pc = s->module->instructions + handler_pc;
 *
 * For body strands (`s->module == NULL`) AND for method-frame try/catch
 * (the handler PC is in the method's proto, not the chunk's module), the
 * dereference loads from a stale or NULL pointer.  On hardware the crash
 * presents as `Guru Meditation Error: LoadProhibited` at uunwind.c:358
 * with EXCVADDR=0x00000000.  Backtrace:
 *
 *   urbi_unwind (uunwind.c:358)
 *   dispatch_loop_until_yield (uvm.c:1914 safepoint)
 *   urbi_step
 *
 * Root cause: `s->pc_base` already tracks the current proto's instructions
 * correctly (set on every OP_CALL, restored in pop_call_frame).  The fix
 * is `s->pc = s->pc_base + handler_pc`.  Universally correct because:
 *   - chunk-top:                 pc_base == module->instructions
 *   - in a method (no nested):   pc_base == method's proto instructions
 *   - throw nested deeper:       walker pops frames first, so pc_base is
 *                                already updated to the TRY_FRAME's proto
 *
 * Same class of bug as task #23 (pop_call_frame) and task #22/S43
 * (ustrand_consts_for_closure) — the early code assumed all execution
 * lives inside a chunk-top module's instructions, but body strands and
 * method frames break that assumption.  Filed as task #24 / S44. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "module/umodule.h"
#include "value/uarena.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

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

/* === Test 1: try/catch inside a method invoked from an at-body ==========
 *
 *   class C { var go = function () { try { throw "x" } catch (e) { ... } } };
 *   Realm.c = C.new();
 *   at (ev?) Realm.c.go()
 *
 * Pre-fix: at-body fires → method invoked (frame pushed) → throw in method
 * body → unwind walker → TRY_FRAME catch absorption at line 358 →
 * s->module->instructions deref with s->module == NULL → NULL deref crash.
 * Post-fix: catch absorbs, side-effect counter increments to 1. */
UTEST(method_try_catch_in_at_body_absorbs)
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
        "class C {"
        "  var go = function () {"
        "    try { throw \"x\" } catch (e) { Realm.n = Realm.n + 1 }"
        "  }"
        "};"
        "Realm.c = C.new();"
        "at (ev?) Realm.c.go()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
    UStepResult step = drain_to_quiescent(&vm);
    UASSERT(step != URBI_STEP_FATAL);

    UValue n = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "n", 1, &n));
    UASSERT_EQ((int)UVAL_INT, (int)n.kind);
    UASSERT_EQ(1LL, n.v.i);

    uarena_destroy(&arena);
    umodule_destroy(&module);
    urbi_vm_destroy(&vm);
}

/* === Test 2: control — try/catch at chunk-top (no method, no at-body).
 * Confirms the catch absorption path itself works on the simplest shape;
 * if this fails the bug is broader than method+at-body. */
UTEST(chunktop_try_catch_absorbs_control)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "n", 1,
                                               utest_e2e_make_int(0)));

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "try { throw \"x\" } catch (e) { Realm.n = Realm.n + 1 }",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    UStepResult step = drain_to_quiescent(&vm);
    UASSERT(step != URBI_STEP_FATAL);

    UValue n = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "n", 1, &n));
    UASSERT_EQ((int)UVAL_INT, (int)n.kind);
    UASSERT_EQ(1LL, n.v.i);

    uarena_destroy(&arena);
    umodule_destroy(&module);
    urbi_vm_destroy(&vm);
}

/* === Test 3: try/catch inside a method called from chunk-top.
 * Frames non-empty (in method) but s->module non-NULL (chunk-top strand).
 * Pre-fix this also crashes — s->module->instructions is the chunk's
 * bytecode, but handler_pc is the offset within the METHOD'S proto.
 * Tests the second axis of the bug. */
UTEST(method_try_catch_from_chunktop)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "n", 1,
                                               utest_e2e_make_int(0)));

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "class C {"
        "  var go = function () {"
        "    try { throw \"x\" } catch (e) { Realm.n = Realm.n + 1 }"
        "  }"
        "};"
        "Realm.c = C.new();"
        "Realm.c.go()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    UStepResult step = drain_to_quiescent(&vm);
    UASSERT(step != URBI_STEP_FATAL);

    UValue n = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "n", 1, &n));
    UASSERT_EQ((int)UVAL_INT, (int)n.kind);
    UASSERT_EQ(1LL, n.v.i);

    uarena_destroy(&arena);
    umodule_destroy(&module);
    urbi_vm_destroy(&vm);
}

void
test_unwind_method_try_catch_suite(void)
{
    utest_run("unwind_method_try_catch: try/catch in method from at-body (task #24)",
              method_try_catch_in_at_body_absorbs);
    utest_run("unwind_method_try_catch: chunk-top try/catch control",
              chunktop_try_catch_absorbs_control);
    utest_run("unwind_method_try_catch: try/catch in method from chunk-top",
              method_try_catch_from_chunktop);
}
