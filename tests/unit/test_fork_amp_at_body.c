/* SPDX-License-Identifier: BSD-3-Clause */
/* test_fork_amp_at_body — probe for the `& Realm.X = ...` at-body
 * pattern not incrementing slots on ESP32-S3-EYE eye_demo 2026-05-16.
 *
 * The eye_demo Tier-2c addition:
 *
 *   at (stats_tick?) Realm.fork_lhs = Realm.fork_lhs + 1 &
 *                    Realm.fork_rhs = Realm.fork_rhs + 1;
 *
 * Both counters stayed at 0 across 14+ stats_tick fires.  Neither
 * incremented.  This test reproduces the shape host-side to see what
 * the parser actually does with this construct. */

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
    for (i = 0; i < 500; i++) {
        r = urbi_step(vm, 500, NULL);
        if (r == URBI_STEP_QUIESCENT || r == URBI_STEP_WAKE_AT ||
            r == URBI_STEP_FATAL) {
            return r;
        }
    }
    return r;
}

/* Test 1: `&` inside a function called from an at-handler.
 * Workaround for the chunk-top OP_FORK_JOIN limitation. */
UTEST(amp_inside_function_called_from_at_handler)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    urbi_event_id_t tick = urbi_event_register(&vm, r, "tick", NULL, NULL);
    UASSERT(tick != URBI_EVENT_ID_INVALID);

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "var fork_lhs = 0; var fork_rhs = 0;"
        "var fork_test = function () {"
        "  Realm.fork_lhs = Realm.fork_lhs + 1 &"
        "  Realm.fork_rhs = Realm.fork_rhs + 1"
        "};"
        "at (tick?) fork_test()",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Fire 3 ticks. */
    int i;
    for (i = 0; i < 3; i++) {
        urbi_inject_event(&vm, (uint32_t)tick, NULL, 0U);
        UStepResult step = drain_to_quiescent(&vm);
        UASSERT(step != URBI_STEP_FATAL);
    }

    UValue lhs = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "fork_lhs", 8, &lhs));

    UValue rhs = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "fork_rhs", 8, &rhs));

    /* Both counters should equal 3 (one per tick fire). */
    UASSERT_EQ(3LL, lhs.v.i);
    UASSERT_EQ(3LL, rhs.v.i);

    uarena_destroy(&arena);
    umodule_destroy(&module);
    urbi_vm_destroy(&vm);
}

/* Test 2: confirm `&` at chunk-top FAILS with the documented error.
 * Documents the runtime constraint: OP_FORK_JOIN requires urbi_step. */
UTEST(amp_at_chunktop_fails_per_spec)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    /* Chunk-top runs via urbi_run_chunk (transient strand context).
     * OP_FORK_JOIN can't run there per its docstring. */
    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "var a = 0; var b = 0;"
        "Realm.a = 1 & Realm.b = 2",
        NULL);
    /* Expected: URBI_ERR_STRAND_FATAL = -2 with documented errmsg. */
    UASSERT_EQ(-2, rc);

    uarena_destroy(&arena);
    umodule_destroy(&module);
    urbi_vm_destroy(&vm);
}

void
test_fork_amp_at_body_suite(void)
{
    utest_run("fork_amp_at_body: `&` inside function from at-handler works",
              amp_inside_function_called_from_at_handler);
    utest_run("fork_amp_at_body: `&` at chunk-top fails (per spec — needs urbi_step)",
              amp_at_chunktop_fails_per_spec);
}
