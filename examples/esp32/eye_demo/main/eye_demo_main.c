/* SPDX-License-Identifier: BSD-3-Clause */
/* eye_demo_main — app_main boot sequence.
 *
 * Three pieces:
 *   1. c_log host fn — urbiscript-visible logging shim routed through ESP_LOGI.
 *   2. destructure_blob — 12-byte ISR payload -> 3 UValue ints for the
 *      "blob_seen" event drain.
 *   3. app_main — canonical 7-step boot order per spec §5.7:
 *        ESP-IDF init (implicit) -> urbi_vm_init -> port hooks ->
 *        events + host fns -> bytecode load -> peripherals -> task spawn.
 */

#include <stddef.h>      /* size_t */
#include <stdint.h>      /* uint32_t */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "urbi/urbi.h"
#include "urbi/types.h"

/* Full UVM struct definition needed because app_main stack/BSS-allocates
 * `static struct UVM vm` directly — the public API only forward-declares
 * the type.  Per docs/embedding-guide.md §"Allocating the VM" the
 * canonical pattern is `#include "vm/uvm.h"` from src/. */
#include "vm/uvm.h"
#include "event/uevent_ring.h"   /* TEMP diag: UEventRing.overflow_count access */

#include "port_esp_idf.h"

#include "eye_button.h"
#include "eye_camera.h"
#include "eye_display.h"

/* Baked bytecode for eye_demo.u — wire-format v1.5 bytes produced at build
 * time by tools/urbi-compile-stdlib --to-header (see main/CMakeLists.txt).
 * Exposes file-local `static const uint8_t eye_demo_bytecode[]` plus
 * matching `static const size_t eye_demo_bytecode_size`; included only here
 * so the symbols stay TU-local and don't leak to the rest of the link. */
#include "eye_demo_bytecode.h"

static const char *TAG = "eye_demo";

/* c_log: urbiscript-visible logging shim.
 *
 * Signature matches urbi_native_method_fn (include/urbi/urbi.h:295) —
 * see eye_display.c:161-188 for the spec-vs-real-API drift note that
 * applies here too.
 *
 * Argument convention: a single UVAL_STR message.  Extra args are
 * ignored; nargs == 0 is a no-op.  Logged at ESP_LOG_INFO via the
 * "eye_demo" tag so menuconfig log-level filters apply uniformly with
 * the camera / display tasks.
 *
 * Returns UEXEC_OK and writes UVAL_NIL into *out.  Never raises.
 *
 * Concurrency: MAIN (script context).  ESP_LOGI is thread-safe under
 * the default vprintf path, but this fn is only called from the urbi
 * task — no cross-task contention to reason about. */
int c_log(struct UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm;
    (void)self;
    if (nargs >= 1U && args != NULL) {
        size_t len = 0U;
        const char *s = urbi_value_as_str(args[0], &len);
        if (s != NULL) {
            ESP_LOGI(TAG, "%.*s", (int)len, s);
        }
    }
    if (out != NULL) {
        *out = urbi_make_nil();
    }
    return UEXEC_OK;
}

/* destructure_blob: ISR-payload -> UValue-args destructure for "blob_seen".
 *
 * Matches the canonical urbi_event_payload_destructure_fn typedef at
 * <urbi/urbi.h>:502-505:
 *
 *     typedef int (*urbi_event_payload_destructure_fn)(
 *         struct UVM *vm,
 *         const urbi_event_payload_t *payload, size_t payload_len,
 *         UValue *out_args, int max_args, void *ud);
 *
 * The plan spec sketch (plans/2026-05-11-v0.7.2-esp32.md T39) used
 * `out` / `max` parameter names — same shape, just different identifiers.
 *
 * Payload contract: eye_camera.c:75-80 writes the centroid + area as three
 * uint32_t channels of the 16-byte urbi_event_payload_t.u32[] union:
 *
 *     p.u32[0] = blob.x       (0..319)
 *     p.u32[1] = blob.y       (0..239)
 *     p.u32[2] = blob.area    (pixel count)
 *     p.u32[3] = 0            (reserved / pad)
 *
 * urbiscript at-binding: `at (blob_seen ?(x, y, area)) { ... }` receives
 * three int args; the rewritten eye_demo.u uses (x, y, area) shape.
 *
 * Returns 3 on success (three UValues written), URBI_ERR_INVALID_ARG (-1)
 * if max_args < 3 or payload_len < 12 — the at-drain falls back to dropping
 * the body args for this occurrence per <urbi/urbi.h>:486-493.
 *
 * Thread safety: MAIN (called from the safepoint drain on the urbi task,
 * NOT from ISR context — the ring queues the raw bytes; this fn runs at
 * the next urbi_step). */
/* TEMP diag counters — defined here (file-scope) and used by the camera
 * task (eye_camera.c) + the stats task (port_urbi_stats_task below).
 * Strip before ship. */
volatile uint32_t g_cam_frames        = 0;
volatile uint32_t g_cam_injects       = 0;
volatile uint32_t g_destruct_blob_n   = 0;

static int destructure_blob(struct UVM *vm,
                            const urbi_event_payload_t *payload,
                            size_t payload_len,
                            UValue *out_args, int max_args, void *ud)
{
    (void)ud;
    g_destruct_blob_n++;
    if (max_args < 3 || payload_len < 12 || payload == NULL || out_args == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    UValue v_x    = urbi_make_int((int64_t)payload->u32[0]);
    UValue v_y    = urbi_make_int((int64_t)payload->u32[1]);
    UValue v_area = urbi_make_int((int64_t)payload->u32[2]);

    out_args[0] = v_x;
    out_args[1] = v_y;
    out_args[2] = v_area;

    /* Side-effect: ALSO write the destructured values to Realm globals so
     * script-side at-handlers can read them.  Required because v0.7.1's
     * async event delivery does not thread the destructured payload into
     * the body strand's R[0] (design-risks row 14) — the eye_demo at-body
     * uses `Realm.last_blob_x` / `_y` to access the values from the safe
     * Realm-slot path instead.  Slot writes are safe here: destructure_blob
     * runs on the urbi task at safepoint drain, not in ISR context. */
    struct URealm *r = urbi_realm_global(vm);
    if (r != NULL) {
        urbi_realm_set_global(vm, r, "last_blob_x",    11U, v_x);
        urbi_realm_set_global(vm, r, "last_blob_y",    11U, v_y);
        urbi_realm_set_global(vm, r, "last_blob_area", 14U, v_area);
    }
    return 3;
}

/* TEMP diag: destructure_button — logs every time the safepoint drain
 * processes a button_pressed event.  Returns 0 args (button payload is
 * empty; at-handler bodies take no args).  Strip before ship. */
static int destructure_button(struct UVM *vm,
                              const urbi_event_payload_t *payload,
                              size_t payload_len,
                              UValue *out_args, int max_args, void *ud)
{
    (void)vm; (void)payload; (void)payload_len;
    (void)out_args; (void)max_args; (void)ud;
    static uint32_t s_drain_count = 0;
    ESP_LOGI(TAG, ">>> safepoint drain: button_pressed (drain=%u)",
             (unsigned)++s_drain_count);
    return 0;
}

/* === app_main: canonical 7-step boot order =====================
 *
 * Per spec §5.7 (docs/superpowers/specs/2026-05-11-m7-wave2-embedding-api-design.md)
 * the ordering is fixed because:
 *
 *   1. Port hooks must be installed BEFORE any event registration (event
 *      drain calls back into urbi_writer/time/wake on first dispatch).
 *   2. Events must be registered BEFORE peripheral init (camera_init /
 *      button_install_isr stash event ids that get dereferenced from ISR
 *      context on the very next interrupt).
 *   3. Host fns must be registered BEFORE bytecode load (the eye_demo
 *      bytecode references log/draw_crosshair/set_target_color globals at
 *      load time — unresolved globals would throw on first dispatch).
 *   4. The urbi task must spawn LAST (it parks on a task-notification
 *      waiting for events, which can only fire after peripherals are up
 *      and wake_fn is installed with the right TaskHandle_t).
 *
 * Several drifts from the plan sketch (plans/2026-05-11-v0.7.2-esp32.md T40):
 *
 *   - The sketch uses `umodule_deserialize + urbi_load_module`; the
 *     canonical v0.7.1 public API is `urbi_module_from_bytes`
 *     (<urbi/urbi.h>:1066) which allocates + deserializes in one step
 *     and pairs with `urbi_load_module` to bind into the VM.
 *
 *   - `urbi_vm_init` returns `int` (URBI_OK / URBI_ERR_OOM) — wrapped in
 *     ESP_ERROR_CHECK with a !=URBI_OK->ESP_FAIL truthiness shim.
 *
 *   - `urbi_set_isr_check_fn` is only declared in URBI_DEBUG builds (see
 *     <urbi/urbi.h>:837 — the macro URBI_ASSERT_NOT_ISR collapses to a
 *     no-op in non-debug builds, so the registration is similarly skipped).
 *
 *   - The urbi task is pinned to core 0 via xTaskCreateStaticPinnedToCore
 *     (not xTaskCreateStatic) so it co-schedules with the camera task on
 *     core 0 per spec §5.1 and leaves the display task alone on core 1.
 */

static struct UVM   vm;
static StackType_t  urbi_stack_buf[URBI_STACK_WORDS];
static StaticTask_t urbi_tcb;
static TaskHandle_t urbi_task_handle;

void app_main(void)
{
    /* Step 1-2: ESP-IDF init is implicit (called by startup code).  Init
     * the VM with the PSRAM-backed allocator from the port glue. */
    int rc = urbi_vm_init(&vm, port_psram_alloc, NULL);
    ESP_ERROR_CHECK(rc == URBI_OK ? ESP_OK : ESP_FAIL);

    /* Route runtime diagnostic messages (URBI_LOG_WARN body throws, spawn
     * OOM, watchdog warnings, etc.) through the ESP_LOG facility under
     * the "urbi-runtime" tag.  Distinct from urbi_set_writer below, which
     * wires the script-side I/O sink. */
    urbi_set_diag_fn(&vm, port_diag_to_esp);

    /* Step 3: install port hooks (writer / time / wake).  The wake_fn
     * binding takes the address of urbi_task_handle so the wake callback
     * can dispatch xTaskNotifyGiveFromISR to the urbi task once spawned
     * — the handle gets filled in at step 7 below. */
    urbi_set_time_us(&vm, port_time_us);
    urbi_set_writer (&vm, port_writer, NULL);
    urbi_set_wake_fn(&vm, port_wake_from_inject, &urbi_task_handle);
#ifdef URBI_DEBUG
    urbi_set_isr_check_fn(&vm, port_in_isr);
#endif

    /* Step 4: realm + events + host fns.  urbi_realm_global auto-creates
     * the VM's global realm on first call (returns NULL only on OOM). */
    struct URealm *r = urbi_realm_global(&vm);
    ESP_ERROR_CHECK(r != NULL ? ESP_OK : ESP_FAIL);

    urbi_event_id_t ev_blob = urbi_event_register(&vm, r, "blob_seen",
                                                  destructure_blob, NULL);
    /* TEMP diag: register destructure_button so we can see whether the
     * urbi safepoint drain ever processes button-pressed events at all
     * (vs being dropped at the ring level).  Compare with the existing
     * ">>> ISR: BOOT button fired" log from button_isr to bisect:
     *   - ISR log AND destructure log → event reaches the drain; if no
     *     subsequent log("RED"/"GREEN"/"BLUE") appears, the at-handler
     *     chain itself is the bug
     *   - ISR log but NO destructure log → event is being dropped
     *     between urbi_inject_event and the safepoint drain
     * Strip before ship. */
    urbi_event_id_t ev_btn  = urbi_event_register(&vm, r, "button_pressed",
                                                  destructure_button, NULL);
    ESP_ERROR_CHECK(ev_blob != URBI_EVENT_ID_INVALID ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(ev_btn  != URBI_EVENT_ID_INVALID ? ESP_OK : ESP_FAIL);

    ESP_ERROR_CHECK(urbi_register(&vm, r, "draw_crosshair",   c_draw_crosshair)   == URBI_OK ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(urbi_register(&vm, r, "set_target_color", c_set_target_color) == URBI_OK ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(urbi_register(&vm, r, "log",              c_log)              == URBI_OK ? ESP_OK : ESP_FAIL);

    /* Step 5: load the baked bytecode.  urbi_module_from_bytes returns
     * NULL on any deserialize failure; ESP_ERROR_CHECK panics so we get
     * a clean coredump rather than a silent boot loop. */
    char errbuf[128] = {0};
    struct UModule *m = urbi_module_from_bytes(eye_demo_bytecode,
                                               eye_demo_bytecode_size,
                                               errbuf, sizeof errbuf);
    if (m == NULL) {
        ESP_LOGE(TAG, "urbi_module_from_bytes failed: %s", errbuf);
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    ESP_ERROR_CHECK(urbi_load_module(&vm, m, "eye_demo") == URBI_OK ? ESP_OK : ESP_FAIL);

    /* Step 6: peripherals.  Each init spawns its own pinned FreeRTOS task
     * (display on core 1, camera on core 0) and stashes the (vm, ev) pair
     * for ISR-time use.  Must run after event registration so the ev_*
     * ids are valid.
     *
     * Order matters: eye_display_init() MUST run before eye_camera_init()
     * because the camera task is pinned to core 0 at the same priority as
     * main_task; once spawned, FreeRTOS round-robins them, and the camera
     * task's first DQBUF + display_post_frame happens before main_task
     * gets back to eye_display_init().  Display's frame_q would still be
     * NULL at that point, and xQueueSend(NULL) asserts in FreeRTOS.
     * Initialize the consumer side (display + frame_q) first; once camera
     * starts producing, the consumer is ready. */
    eye_display_init();
    eye_camera_init(&vm, ev_blob);
    button_install_isr(&vm, ev_btn);

    /* Step 7: spawn the urbi task pinned to core 0 (co-scheduling with
     * camera per spec §5.1).  Priority +2 above idle; +1 above the camera
     * and display tasks so urbi drains events ahead of new captures. */
    urbi_task_handle = xTaskCreateStaticPinnedToCore(
        port_urbi_task_body, "urbi", URBI_STACK_WORDS, &vm,
        tskIDLE_PRIORITY + 2, urbi_stack_buf, &urbi_tcb, 0);
    ESP_ERROR_CHECK(urbi_task_handle != NULL ? ESP_OK : ESP_FAIL);

    ESP_LOGI(TAG, "boot complete: urbi task spawned (handle=%p)", (void *)urbi_task_handle);

    /* TEMP diag: periodic stats dump every 3 s.  Lets us see whether
     * camera/destructure/at-handler are still firing after a wedge.
     * Strip before ship. */
    extern volatile uint32_t g_crosshair_call_count;  /* eye_display.c */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        UEventRing *ring = vm.event_ring;
        ESP_LOGI(TAG, "STATS: cam_frames=%u cam_injects=%u "
                       "destruct_blob=%u crosshair=%u ring_overflow=%u "
                       "strand_runnable=%u watcher_in_use=%u/%u",
                 (unsigned)g_cam_frames,
                 (unsigned)g_cam_injects,
                 (unsigned)g_destruct_blob_n,
                 (unsigned)g_crosshair_call_count,
                 (unsigned)(ring ? ring->overflow_count : 0U),
                 (unsigned)vm.strand_runnable_count,
                 (unsigned)vm.watcher_pool_in_use,
                 (unsigned)vm.watcher_pool_high_water);
    }
}
