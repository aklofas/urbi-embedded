/* SPDX-License-Identifier: BSD-3-Clause */
/* test_parse_block_in_at_body — S47 regression.
 *
 * Original urbi spec accepts `{ stmts }` as a body for at-handlers,
 * onleave handlers, whenever bodies, and any inner-tier position.
 * Examples from `legacy/repos/aldebaran-urbi/tests/2.x/at/` .chk files:
 *
 *   at (e?) { ... }
 *   at (cond) { stmts } onleave { ... }
 *   at (Event.new()?) {}
 *
 * Pre-S47, our parser rejected `at (e?) { ... }` with "expected
 * expression" pointed at the first statement inside the block.  Root
 * cause: `parse_statement_or_expr` (used for at-bodies / onleave /
 * whenever bodies / etc.) had no LBRACE handler — fell through to
 * `parse_inner_tier` → `parse_expression`, neither of which accepts
 * LBRACE as an expression prefix.
 *
 * Surfaced 2026-05-16 by eye_demo's attempt to collapse three chained
 * `at (blob_seen?)` handlers into one multi-statement at-body via the
 * canonical brace-block form. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "module/umodule.h"
#include "value/uarena.h"

#include <stddef.h>
#include <stdint.h>

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

/* === Test 1: at-event with brace-block body parses + runs. */
UTEST(at_event_brace_block_body_compiles)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t ev = urbi_event_register(&vm, r, "ev", NULL, NULL);
    UASSERT(ev != URBI_EVENT_ID_INVALID);

    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "a", 1,
                                               utest_e2e_make_int(0)));
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "b", 1,
                                               utest_e2e_make_int(0)));
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "c", 1,
                                               utest_e2e_make_int(0)));

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    /* Three-statement brace-block at-body — the eye_demo idiom. */
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "at (ev?) {"
        "  Realm.a = Realm.a + 1;"
        "  Realm.b = Realm.b + 10;"
        "  Realm.c = Realm.c + 100"
        "}",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
    UStepResult step = drain_to_quiescent(&vm);
    UASSERT(step != URBI_STEP_FATAL);

    UValue a = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "a", 1, &a));
    UASSERT_EQ((int)UVAL_INT, (int)a.kind);
    UASSERT_EQ(1LL, a.v.i);

    UValue b = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "b", 1, &b));
    UASSERT_EQ((int)UVAL_INT, (int)b.kind);
    UASSERT_EQ(10LL, b.v.i);

    UValue c = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "c", 1, &c));
    UASSERT_EQ((int)UVAL_INT, (int)c.kind);
    UASSERT_EQ(100LL, c.v.i);

    uarena_destroy(&arena);
    umodule_destroy(&module, NULL);
    urbi_vm_destroy(&vm);
}

/* === Test 2: at-cond with brace-block body. */
UTEST(at_cond_brace_block_body_compiles)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "x", 1,
                                               utest_e2e_make_int(0)));
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "out", 3,
                                               utest_e2e_make_int(0)));

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "at (Realm.x > 3) {"
        "  Realm.out = Realm.out + 1"
        "};"
        "var t = function () { Realm.x = 5 }; t()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    UStepResult step = drain_to_quiescent(&vm);
    UASSERT(step != URBI_STEP_FATAL);

    UValue out = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "out", 3, &out));
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(1LL, out.v.i);

    uarena_destroy(&arena);
    umodule_destroy(&module, NULL);
    urbi_vm_destroy(&vm);
}

/* === Test 3: whenever with brace-block body. */
UTEST(whenever_brace_block_body_compiles)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "x", 1,
                                               utest_e2e_make_int(0)));
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, r, "fired", 5,
                                               utest_e2e_make_int(0)));

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "whenever (Realm.x > 3) {"
        "  Realm.fired = Realm.fired + 1"
        "};"
        "var t = function () { Realm.x = 5 }; t()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    UStepResult step = drain_to_quiescent(&vm);
    UASSERT(step != URBI_STEP_FATAL);

    UValue fired = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "fired", 5, &fired));
    UASSERT_EQ((int)UVAL_INT, (int)fired.kind);
    UASSERT(fired.v.i >= 1);

    uarena_destroy(&arena);
    umodule_destroy(&module, NULL);
    urbi_vm_destroy(&vm);
}

/* === Test 4: at-event with brace-block onleave handler. */
UTEST(at_event_brace_onleave_handler_compiles)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    /* Parse-only smoke test — onleave on event watchers doesn't fire
     * via inject_event today (event has no falling edge); the parse-
     * and-emit success is the contract being asserted. */
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "var x = 1;"
        "at (Realm.x > 0) {"
        "  Realm.x = 1"
        "} onleave {"
        "  Realm.x = 0"
        "}",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    uarena_destroy(&arena);
    umodule_destroy(&module, NULL);
    urbi_vm_destroy(&vm);
}

void
test_parse_block_in_at_body_suite(void)
{
    utest_run("parse_block_in_at_body: at-event with brace-block body (S47)",
              at_event_brace_block_body_compiles);
    utest_run("parse_block_in_at_body: at-cond with brace-block body",
              at_cond_brace_block_body_compiles);
    utest_run("parse_block_in_at_body: whenever with brace-block body",
              whenever_brace_block_body_compiles);
    utest_run("parse_block_in_at_body: at-event with brace-block onleave",
              at_event_brace_onleave_handler_compiles);
}
