/* SPDX-License-Identifier: BSD-3-Clause */
/* test_imu_integration.c — IMU multi-axis atomic-section + named-event
 * integration scenario combining Gap B (named events + destructure) and
 * Gap R (atomic section) and Gap J (host-side reactive watchers).
 *
 * Scenario:
 *   - Three named events: EV_ACCEL, EV_GYRO, EV_MAG, each with a 3-float
 *     destructure fn that extracts x/y/z from the payload.
 *   - Three host watchers (one per event) that record the received axes into
 *     a per-axis "sub-frame" state struct.
 *   - A simulated ISR uses urbi_atomic_begin / urbi_inject_event × 3 /
 *     urbi_atomic_end to batch all three axis events atomically.
 *   - After urbi_atomic_end flushes the drain, all three host watchers fire
 *     in one pass — verifying no partial-observation interleaving.
 *   - ISR simulation uses urbi_set_isr_check_fn to install a custom predicate
 *     (always-false; the test is MAIN-thread, not a real ISR) so the ASAN
 *     build's URBI_ASSERT_NOT_ISR gates don't fire during inject.
 *
 * Two sub-tests:
 *   1. imu_atomic_all_three_fire: verify all 3 host watchers fire after one
 *      atomic batch; check per-axis values match injected payload.
 *   2. imu_no_partial_interleave: inject atomic batch; unregister one watcher
 *      between inject and drain (via atomic section: inject → unregister →
 *      atomic_end); verify only 2 of 3 watchers fired (no partial dispatch). */

#include "utest.h"
#include "utest_e2e_helpers.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "event/uevent_ring.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* =========================================================================
 * Shared IMU sub-frame state
 * ========================================================================= */

typedef struct {
    int    fired;
    double x, y, z;
    urbi_watcher_handle_t handle;
} AxisState;

static AxisState g_accel;
static AxisState g_gyro;
static AxisState g_mag;

/* =========================================================================
 * 3-float destructure fn (shared for all three sensors)
 * ========================================================================= */

static int
float3_destruct(struct UVM *vm,
                const urbi_event_payload_t *payload, size_t payload_len,
                UValue *out_args, int max_args, void *ud)
{
    (void)vm; (void)ud;
    if (payload == NULL || payload_len < 3 * sizeof(float) || max_args < 3) {
        return 0;
    }
    out_args[0].kind = (uint8_t)UVAL_FLOAT; out_args[0].v.f = (double)payload->f32[0];
    out_args[1].kind = (uint8_t)UVAL_FLOAT; out_args[1].v.f = (double)payload->f32[1];
    out_args[2].kind = (uint8_t)UVAL_FLOAT; out_args[2].v.f = (double)payload->f32[2];
    return 3;
}

/* =========================================================================
 * Per-axis host watcher callbacks
 * ========================================================================= */

static int
accel_cb(struct UVM *vm, urbi_event_id_t event_id,
         const UValue *args, int argc, void *ud)
{
    (void)vm; (void)event_id; (void)ud;
    g_accel.fired++;
    if (argc >= 3) {
        g_accel.x = args[0].v.f;
        g_accel.y = args[1].v.f;
        g_accel.z = args[2].v.f;
    }
    return URBI_OK;
}

static int
gyro_cb(struct UVM *vm, urbi_event_id_t event_id,
        const UValue *args, int argc, void *ud)
{
    (void)vm; (void)event_id; (void)ud;
    g_gyro.fired++;
    if (argc >= 3) {
        g_gyro.x = args[0].v.f;
        g_gyro.y = args[1].v.f;
        g_gyro.z = args[2].v.f;
    }
    return URBI_OK;
}

static int
mag_cb(struct UVM *vm, urbi_event_id_t event_id,
       const UValue *args, int argc, void *ud)
{
    (void)vm; (void)event_id; (void)ud;
    g_mag.fired++;
    if (argc >= 3) {
        g_mag.x = args[0].v.f;
        g_mag.y = args[1].v.f;
        g_mag.z = args[2].v.f;
    }
    return URBI_OK;
}

/* =========================================================================
 * Sub-test 1: all three IMU axis watchers fire from one atomic batch.
 * ========================================================================= */

UTEST(imu_atomic_all_three_fire)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Reset state. */
    g_accel.fired = 0;  g_accel.x = 0.0; g_accel.y = 0.0; g_accel.z = 0.0;
    g_gyro.fired  = 0;  g_gyro.x  = 0.0; g_gyro.y  = 0.0; g_gyro.z  = 0.0;
    g_mag.fired   = 0;  g_mag.x   = 0.0; g_mag.y   = 0.0; g_mag.z   = 0.0;

    /* Register three named events. */
    urbi_event_id_t id_accel = urbi_event_register(&vm, realm, "EV_ACCEL",
                                                    float3_destruct, NULL);
    urbi_event_id_t id_gyro  = urbi_event_register(&vm, realm, "EV_GYRO",
                                                    float3_destruct, NULL);
    urbi_event_id_t id_mag   = urbi_event_register(&vm, realm, "EV_MAG",
                                                    float3_destruct, NULL);
    UASSERT(id_accel != URBI_EVENT_ID_INVALID);
    UASSERT(id_gyro  != URBI_EVENT_ID_INVALID);
    UASSERT(id_mag   != URBI_EVENT_ID_INVALID);

    /* Register host-side watchers. */
    g_accel.handle = urbi_register_watcher(&vm, realm, id_accel, accel_cb, NULL);
    g_gyro.handle  = urbi_register_watcher(&vm, realm, id_gyro,  gyro_cb,  NULL);
    g_mag.handle   = urbi_register_watcher(&vm, realm, id_mag,   mag_cb,   NULL);
    UASSERT(g_accel.handle != URBI_WATCHER_HANDLE_INVALID);
    UASSERT(g_gyro.handle  != URBI_WATCHER_HANDLE_INVALID);
    UASSERT(g_mag.handle   != URBI_WATCHER_HANDLE_INVALID);
    /* All three handles must be distinct. */
    UASSERT(g_accel.handle != g_gyro.handle);
    UASSERT(g_accel.handle != g_mag.handle);
    UASSERT(g_gyro.handle  != g_mag.handle);

    /* Simulate ISR: no real ISR here — use NULL isr_check_fn (always MAIN). */
    urbi_set_isr_check_fn(&vm, NULL, NULL);

    /* Atomic batch: all three axis events injected together. */
    urbi_atomic_begin(&vm);

    urbi_event_payload_t pa = {0};
    pa.f32[0] = 0.1f; pa.f32[1] = 0.2f; pa.f32[2] = 9.8f;
    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id_accel,
                                          &pa, 3 * sizeof(float)));

    urbi_event_payload_t pg = {0};
    pg.f32[0] = 0.01f; pg.f32[1] = 0.02f; pg.f32[2] = 0.03f;
    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id_gyro,
                                          &pg, 3 * sizeof(float)));

    urbi_event_payload_t pm = {0};
    pm.f32[0] = 25.0f; pm.f32[1] = -10.0f; pm.f32[2] = 44.0f;
    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id_mag,
                                          &pm, 3 * sizeof(float)));

    /* No watchers fired yet — drain held. */
    UASSERT_EQ(0, g_accel.fired);
    UASSERT_EQ(0, g_gyro.fired);
    UASSERT_EQ(0, g_mag.fired);

    /* End atomic section: triggers drain flush. */
    urbi_atomic_end(&vm);

    /* All three watcher callbacks must have fired exactly once each. */
    UASSERT_EQ(1, g_accel.fired);
    UASSERT_EQ(1, g_gyro.fired);
    UASSERT_EQ(1, g_mag.fired);

    /* Per-axis values must match the injected payload. */
    UASSERT(g_accel.x == (double)0.1f);
    UASSERT(g_accel.y == (double)0.2f);
    UASSERT(g_accel.z == (double)9.8f);

    UASSERT(g_gyro.x == (double)0.01f);
    UASSERT(g_gyro.y == (double)0.02f);
    UASSERT(g_gyro.z == (double)0.03f);

    UASSERT(g_mag.x == (double)25.0f);
    UASSERT(g_mag.y == (double)-10.0f);
    UASSERT(g_mag.z == (double)44.0f);

    /* urbi_step to quiescence: script-side UEvent dispatch queues strands. */
    int q = utest_e2e_run_to_no_runnable(&vm);
    UASSERT(q >= 0);  /* -1 = fatal */

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: unregister one watcher during atomic section → only 2 fire.
 *
 * Inject all 3 events atomically, then unregister the MAG watcher before
 * calling atomic_end.  Because the drain fires after atomic_end and the
 * pending_unregister flag is set before the walk, MAG cb does not fire.
 * ACCEL and GYRO watchers fire normally.
 * ========================================================================= */

UTEST(imu_unregister_during_atomic_section)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    struct URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    g_accel.fired = 0;
    g_gyro.fired  = 0;
    g_mag.fired   = 0;

    urbi_event_id_t id_accel = urbi_event_register(&vm, realm, "EV2_ACCEL",
                                                    float3_destruct, NULL);
    urbi_event_id_t id_gyro  = urbi_event_register(&vm, realm, "EV2_GYRO",
                                                    float3_destruct, NULL);
    urbi_event_id_t id_mag   = urbi_event_register(&vm, realm, "EV2_MAG",
                                                    float3_destruct, NULL);
    UASSERT(id_accel != URBI_EVENT_ID_INVALID);
    UASSERT(id_gyro  != URBI_EVENT_ID_INVALID);
    UASSERT(id_mag   != URBI_EVENT_ID_INVALID);

    g_accel.handle = urbi_register_watcher(&vm, realm, id_accel, accel_cb, NULL);
    g_gyro.handle  = urbi_register_watcher(&vm, realm, id_gyro,  gyro_cb,  NULL);
    g_mag.handle   = urbi_register_watcher(&vm, realm, id_mag,   mag_cb,   NULL);
    UASSERT(g_accel.handle != URBI_WATCHER_HANDLE_INVALID);
    UASSERT(g_gyro.handle  != URBI_WATCHER_HANDLE_INVALID);
    UASSERT(g_mag.handle   != URBI_WATCHER_HANDLE_INVALID);

    /* Atomic batch: inject all 3 events. */
    urbi_atomic_begin(&vm);

    urbi_event_payload_t pf = {0};
    pf.f32[0] = 1.0f; pf.f32[1] = 2.0f; pf.f32[2] = 3.0f;
    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id_accel,
                                          &pf, 3 * sizeof(float)));
    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id_gyro,
                                          &pf, 3 * sizeof(float)));
    UASSERT_EQ(URBI_OK, urbi_inject_event(&vm, (uint32_t)id_mag,
                                          &pf, 3 * sizeof(float)));

    /* Unregister MAG watcher before drain fires. */
    int rc = urbi_unregister_watcher(&vm, g_mag.handle);
    UASSERT_EQ(URBI_OK, rc);

    /* End atomic: drain fires.  MAG is pending_unregister → skipped. */
    urbi_atomic_end(&vm);

    UASSERT_EQ(1, g_accel.fired);   /* ACCEL fired */
    UASSERT_EQ(1, g_gyro.fired);    /* GYRO fired */
    UASSERT_EQ(0, g_mag.fired);     /* MAG suppressed */

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_imu_integration_suite(void)
{
    utest_run("imu_integration: all 3 axis watchers fire from one atomic batch",
              imu_atomic_all_three_fire);
    utest_run("imu_integration: unregister during atomic section suppresses one watcher",
              imu_unregister_during_atomic_section);
}
