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

void
test_pipe_middle_stmt_suite(void)
{
    utest_run("pipe_middle_stmt: 3 chunk-top pipe writes all happen",
              pipe_three_slot_writes_chunktop_passes);
    utest_run("pipe_middle_stmt: 3 at-body pipe writes all happen",
              pipe_three_slot_writes_in_at_body);
}
