/* SPDX-License-Identifier: BSD-3-Clause */
/* test_pipe_middle_stmt — investigation of the SEP_PIPE middle-statement
 * drop observed on ESP32-S3-EYE eye_demo 2026-05-16.
 *
 * Hardware evidence (pre-S47 pipe-form workaround):
 *
 *   at (blob_seen?) draw_crosshair(Realm.last_blob_x, Realm.last_blob_y) |
 *                   Realm.blob_count = Realm.blob_count + 1 |
 *                   Realm.fires_blob = Realm.fires_blob + 1;
 *
 * Over ~20s of blob events:
 *   - draw_crosshair fired (crosshair visible on display)
 *   - Realm.fires_blob climbed 50→354 (3rd stmt)
 *   - Realm.blob_count STAYED AT 0 (2nd stmt silently dropped)
 *   - No MILESTONE fired
 *
 * Host-side reproduction attempts:
 *   - Chunk-top pipe-joined Realm slot writes: works as expected
 *   - At-body pipe-joined Realm slot writes (mirroring the demo shape):
 *     compiles and runs without error.  Body fires correctly.  Cannot
 *     reproduce the dropped middle statement on host.
 *
 * Conclusion 2026-05-16: bug is real on hardware but does not reproduce
 * in isolated host-side tests.  May depend on at-event body context
 * combined with a specific sequence (host-fn + slot write + slot write)
 * AND interaction with the embedded scheduler.  Filed in design-risks
 * for later investigation; worked around by switching to the canonical
 * brace-block at-body form (S47).
 *
 * These tests document that the BASIC pipe-form shapes work — the bug
 * is something subtle in the at-body + host-fn-in-pipe combination
 * that we haven't isolated. */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "module/umodule.h"
#include "value/uarena.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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

/* === Baseline: 3-stmt pipe chain at chunk-top runs all 3 writes. ====== */
UTEST(pipe_three_slot_writes_chunktop_passes)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "var a = 0; var b = 0; var c = 0;"
        "Realm.a = 1 | Realm.b = 2 | Realm.c = 3",
        NULL);
    if (rc != URBI_OK) {
        printf("    [pipe-chunktop] rc=%d, last_errmsg='%s'\n",
               rc, vm.last_errmsg);
    }
    UASSERT_EQ(URBI_OK, rc);

    UValue a = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "a", 1, &a));
    UASSERT_EQ(1LL, a.v.i);

    UValue b = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "b", 1, &b));
    /* The interesting assertion: middle statement.  Hardware showed
     * a similar middle write being dropped in at-body context; this
     * confirms the chunk-top shape works. */
    UASSERT_EQ(2LL, b.v.i);

    UValue c = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "c", 1, &c));
    UASSERT_EQ(3LL, c.v.i);

    uarena_destroy(&arena);
    umodule_destroy(&module);
    urbi_vm_destroy(&vm);
}

/* === At-body pipe form runs all 3 writes (mirrors eye_demo shape). ==== */
UTEST(pipe_three_slot_writes_in_at_body)
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
        "var a = 0; var b = 0; var c = 0;"
        "at (ev?) Realm.a = Realm.a + 1 |"
        "         Realm.b = Realm.b + 1 |"
        "         Realm.c = Realm.c + 1",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    urbi_inject_event(&vm, (uint32_t)ev, NULL, 0U);
    UStepResult step = drain_to_quiescent(&vm);
    UASSERT(step != URBI_STEP_FATAL);

    UValue a = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "a", 1, &a));
    UASSERT_EQ(1LL, a.v.i);

    UValue b = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "b", 1, &b));
    UASSERT_EQ(1LL, b.v.i);

    UValue c = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "c", 1, &c));
    UASSERT_EQ(1LL, c.v.i);

    uarena_destroy(&arena);
    umodule_destroy(&module);
    urbi_vm_destroy(&vm);
}

/* === var-decl `=` RHS doesn't absorb pipe (S48-followup). =============
 * Legacy spec: `var x = 1 | y = 2` parses as `(var x = 1) | (y = 2)`.
 * See legacy/repos/aldebaran-urbi/tests/2.x/atomic.chk for the
 * `var n = 0 | {};` pattern. */
UTEST(var_decl_pipe_does_not_absorb_rhs)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "var x = 1 | var y = 2 | var z = 3",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    UValue x = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "x", 1, &x));
    UASSERT_EQ(1LL, x.v.i);

    UValue y = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "y", 1, &y));
    UASSERT_EQ(2LL, y.v.i);

    UValue z = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "z", 1, &z));
    UASSERT_EQ(3LL, z.v.i);

    uarena_destroy(&arena);
    umodule_destroy(&module);
    urbi_vm_destroy(&vm);
}

/* === local-var `=` RHS doesn't absorb pipe (S48-followup). =========== */
UTEST(local_assign_pipe_does_not_absorb_rhs)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    URealm *r = urbi_realm_global(&vm);
    UASSERT(r != NULL);

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    int rc = utest_e2e_compile_and_run_with_module(&vm, &arena, &module,
        "var a = 0; var b = 0; var c = 0;"
        "a = 10 | b = 20 | c = 30",
        NULL);
    UASSERT_EQ(URBI_OK, rc);

    UValue a = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "a", 1, &a));
    UASSERT_EQ(10LL, a.v.i);

    UValue b = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "b", 1, &b));
    UASSERT_EQ(20LL, b.v.i);

    UValue c = utest_e2e_make_nil();
    UASSERT_EQ(URBI_OK, urbi_realm_get_global(&vm, r, "c", 1, &c));
    UASSERT_EQ(30LL, c.v.i);

    uarena_destroy(&arena);
    umodule_destroy(&module);
    urbi_vm_destroy(&vm);
}

void
test_pipe_middle_stmt_suite(void)
{
    utest_run("pipe_middle_stmt: 3 chunk-top pipe writes all happen",
              pipe_three_slot_writes_chunktop_passes);
    utest_run("pipe_middle_stmt: 3 at-body pipe writes all happen",
              pipe_three_slot_writes_in_at_body);
    utest_run("pipe_middle_stmt: var-decl `=` RHS doesn't absorb pipe (S48-followup)",
              var_decl_pipe_does_not_absorb_rhs);
    utest_run("pipe_middle_stmt: local-assign `=` RHS doesn't absorb pipe (S48-followup)",
              local_assign_pipe_does_not_absorb_rhs);
}
