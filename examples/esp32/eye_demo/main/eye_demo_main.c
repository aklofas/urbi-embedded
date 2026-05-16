/* SPDX-License-Identifier: BSD-3-Clause */
/* eye_demo_main — minimal C host for the urbiscript-heavy ESP32-S3-EYE
 * blob tracker.
 *
 * C side keeps the absolute minimum: VM init + port hooks + event
 * registration + bytecode load + three single-purpose host fns +
 * `destructure_blob` (workaround for the v0.7.1 R[0] payload gap, so
 * urbiscript at-handlers can read the blob centroid from Realm slots).
 *
 * Every piece of demo logic — colour palette, zone hit counters, cycle
 * state, reactive event dispatch — lives in `eye_demo.u`. */
#include <stddef.h>      /* size_t */
#include <stdint.h>      /* int64_t */
#include <stdio.h>       /* snprintf — c_log multi-type formatter */
#include <string.h>      /* memcpy — c_log copy-string path */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "esp_heap_caps.h"   /* heap_caps_get_free_size — stats task */

#include "urbi/urbi.h"
#include "urbi/types.h"

/* Full UVM struct definition needed because app_main static-allocates
 * `static struct UVM vm` directly — the public API only forward-declares
 * the type.  Per docs/embedding-guide.md §"Allocating the VM" the
 * canonical pattern is `#include "vm/uvm.h"` from src/. */
#include "vm/uvm.h"
#include "event/uevent_ring.h"   /* UEventRing.overflow_count — stats task */

#include "port_esp_idf.h"

#include "eye_button.h"
#include "eye_camera.h"
#include "eye_display.h"

/* Baked bytecode for eye_demo.u — wire-format v1.5 bytes produced at
 * build time by tools/urbi-compile-stdlib.  Exposes file-local
 * `static const uint8_t eye_demo_bytecode[]` plus matching
 * `static const size_t eye_demo_bytecode_size`. */
#include "eye_demo_bytecode.h"

static const char *TAG = "eye_demo";

/* c_log: urbiscript-visible logging shim, routed through ESP_LOGI.
 *
 * Variadic + multi-type: walks args[0..nargs), formats each by kind
 * (UVAL_STR / UVAL_INT / UVAL_FLOAT / UVAL_BOOL / UVAL_NIL — anything
 * else dumped as "<kind=N>"), space-separates them, single ESP_LOGI
 * line per call.  Truncates at 160 bytes; the urbi side never builds
 * messages longer than that.
 *
 * Argument convention: `log(x)`, `log("name:", value)`, `log(a, b, c)`
 * — anything goes.  nargs == 0 prints an empty line.
 *
 * Returns UEXEC_OK and writes UVAL_NIL into *out.  Never raises.
 *
 * Concurrency: MAIN (script context).  ESP_LOGI is thread-safe via
 * the default vprintf path; this fn is only called from the urbi task
 * so there's no cross-task contention to reason about. */
int c_log(struct UVM *vm, UValue self,
          UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self;

    char buf[160];
    size_t off = 0U;
    for (uint8_t i = 0U; i < nargs && off + 1U < sizeof buf; i++) {
        if (i > 0U) {
            buf[off++] = ' ';
            if (off + 1U >= sizeof buf) break;
        }
        UValue v = args[i];
        size_t room = sizeof buf - off;
        int written = 0;
        switch ((int)v.kind) {
        case UVAL_STR: {
            size_t len = 0U;
            const char *s = urbi_value_as_str(v, &len);
            if (s != NULL) {
                if (len > room - 1U) len = room - 1U;
                memcpy(buf + off, s, len);
                written = (int)len;
            }
            break;
        }
        case UVAL_INT:
            written = snprintf(buf + off, room, "%lld",
                               (long long)urbi_value_as_int(v));
            break;
        case UVAL_FLOAT:
            written = snprintf(buf + off, room, "%g",
                               urbi_value_as_float(v));
            break;
        case UVAL_BOOL:
            written = snprintf(buf + off, room, "%s",
                               urbi_value_as_bool(v) ? "true" : "false");
            break;
        case UVAL_NIL:
            written = snprintf(buf + off, room, "nil");
            break;
        default:
            written = snprintf(buf + off, room, "<kind=%d>", (int)v.kind);
            break;
        }
        if (written < 0) break;
        off += (size_t)written;
        if (off >= sizeof buf) { off = sizeof buf - 1U; break; }
    }
    buf[off] = '\0';
    ESP_LOGI(TAG, "%s", buf);

    if (out != NULL) *out = urbi_make_nil();
    return UEXEC_OK;
}

/* destructure_blob: ISR-payload -> Realm-slot fan-out for "blob_seen".
 *
 * Matches urbi_event_payload_destructure_fn (include/urbi/urbi.h).
 * Payload contract (eye_camera.c writes this):
 *   p.u32[0] = blob.x       (0..239)
 *   p.u32[1] = blob.y       (0..239)
 *   p.u32[2] = blob.area    (pixel count)
 *   p.u32[3] = 0            (reserved / pad)
 *
 * v0.7.1 async event delivery does not thread the destructured payload
 * into the body strand's R[0] (design-risks row 14).  Workaround:
 * write the three values into Realm.last_blob_x / _y / _area; script
 * at-handlers read them from there.  Slot writes are safe here —
 * destructure_blob runs on the urbi task at safepoint drain, never
 * in ISR context.
 *
 * Returns 0 (no UValue args produced).  Skipping the args allocation
 * is the right move now that nothing reads them — saves three
 * urbi_make_int calls per blob event.
 *
 * Thread safety: MAIN. */
static int destructure_blob(struct UVM *vm,
                            const urbi_event_payload_t *payload,
                            size_t payload_len,
                            UValue *out_args, int max_args, void *ud)
{
    (void)out_args; (void)max_args; (void)ud;
    if (payload == NULL || payload_len < 12U) return 0;
    struct URealm *r = urbi_realm_global(vm);
    if (r != NULL) {
        urbi_realm_set_global(vm, r, "last_blob_x",    11U,
                              urbi_make_int((int64_t)payload->u32[0]));
        urbi_realm_set_global(vm, r, "last_blob_y",    11U,
                              urbi_make_int((int64_t)payload->u32[1]));
        urbi_realm_set_global(vm, r, "last_blob_area", 14U,
                              urbi_make_int((int64_t)payload->u32[2]));
    }
    return 0;
}

/* === Live stats =====================================================
 *
 * Camera task increments g_cam_frames per captured frame and
 * g_cam_injects per emitted blob_seen event (see eye_camera.c).
 * The stats task below polls these + reads Realm.blob_count
 * from urbiscript via urbi_realm_get_global, computes deltas
 * since the last tick, and prints a one-line summary every
 * STATS_PERIOD_MS. */
volatile uint32_t g_cam_frames  = 0;
volatile uint32_t g_cam_injects = 0;

#define STATS_PERIOD_MS  2000

/* Reads from another task — only touch:
 *   - C-side volatile uint32_t counters (camera-task increments)
 *   - VM uint16/uint32 fields used as monitoring gauges
 *     (strand_runnable_count, watcher_pool_in_use, ring->overflow_count)
 *
 * urbi_realm_get_global / urbi_slot_get / any other VM API is NOT
 * safe to call from a non-urbi-task without quiescing the runtime;
 * the v0.7.1 contract is single-threaded.  Indirect signals (the
 * camera/inject FPS, strand/watcher pool occupancy, ring overflow)
 * are sufficient to see whether the urbi pipeline is healthy. */
static void port_stats_task(void *arg)
{
    struct UVM *vm = (struct UVM *)arg;

    uint32_t prev_cam     = 0;
    uint32_t prev_injects = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATS_PERIOD_MS));

        uint32_t cam = g_cam_frames;
        uint32_t inj = g_cam_injects;

        uint32_t cam_fps    = (cam - prev_cam) * 1000U / STATS_PERIOD_MS;
        uint32_t inject_fps = (inj - prev_injects) * 1000U / STATS_PERIOD_MS;

        UEventRing *ring = vm->event_ring;
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t free_psram    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

        ESP_LOGI(TAG,
            "stats: cam=%ufps inject=%ufps "
            "strands=%u watchers=%u/%u ring_ovf=%u "
            "heap_int=%uKB heap_psram=%uKB",
            (unsigned)cam_fps, (unsigned)inject_fps,
            (unsigned)vm->strand_runnable_count,
            (unsigned)vm->watcher_pool_in_use,
            (unsigned)vm->watcher_pool_high_water,
            (unsigned)(ring ? ring->overflow_count : 0U),
            (unsigned)(free_internal / 1024U),
            (unsigned)(free_psram    / 1024U));

        prev_cam     = cam;
        prev_injects = inj;
    }
}

/* === app_main: canonical 7-step boot order ===========================
 *
 * Per spec §5.7 the ordering is fixed because:
 *
 *   1. Port hooks must be installed BEFORE any event registration
 *      (event drain calls back into writer/time/wake/diag on first
 *      dispatch).
 *   2. Events must be registered BEFORE peripheral init (camera_init
 *      / button_install_isr stash event ids dereferenced from ISR
 *      context on the very next interrupt).
 *   3. Host fns must be registered BEFORE bytecode load (eye_demo.u
 *      references log / draw_crosshair / set_target_color at load
 *      time — unresolved globals would throw on first dispatch).
 *   4. The urbi task must spawn LAST (parks on a task notification
 *      that can only fire after peripherals are up and wake_fn is
 *      installed with the right TaskHandle_t).
 *
 * The urbi task is pinned to core 0 via xTaskCreateStaticPinnedToCore
 * so it co-schedules with the camera task per spec §5.1 and leaves
 * the display task alone on core 1. */

static struct UVM   vm;
static StackType_t  urbi_stack_buf[URBI_STACK_WORDS];
static StaticTask_t urbi_tcb;
static TaskHandle_t urbi_task_handle;

void app_main(void)
{
    /* 1-2: VM init with PSRAM-backed allocator. */
    int rc = urbi_vm_init(&vm, port_psram_alloc, NULL);
    ESP_ERROR_CHECK(rc == URBI_OK ? ESP_OK : ESP_FAIL);

    /* 3: port hooks.  urbi_set_diag_fn routes runtime warnings
     * (body throws, spawn OOM, watchdog) through ESP_LOG; without it
     * these would drop on the floor since host_log_fn defaults NULL. */
    urbi_set_diag_fn(&vm, port_diag_to_esp);
    urbi_set_time_us(&vm, port_time_us);
    urbi_set_writer (&vm, port_writer, NULL);
    urbi_set_wake_fn(&vm, port_wake_from_inject, &urbi_task_handle);
#ifdef URBI_DEBUG
    urbi_set_isr_check_fn(&vm, port_in_isr);
#endif

    /* 4: realm + events + host fns. */
    struct URealm *r = urbi_realm_global(&vm);
    ESP_ERROR_CHECK(r != NULL ? ESP_OK : ESP_FAIL);

    urbi_event_id_t ev_blob = urbi_event_register(&vm, r, "blob_seen",
                                                  destructure_blob, NULL);
    urbi_event_id_t ev_btn  = urbi_event_register(&vm, r, "button_pressed",
                                                  NULL, NULL);
    ESP_ERROR_CHECK(ev_blob != URBI_EVENT_ID_INVALID ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(ev_btn  != URBI_EVENT_ID_INVALID ? ESP_OK : ESP_FAIL);

    ESP_ERROR_CHECK(urbi_register(&vm, r, "draw_crosshair",   c_draw_crosshair)   == URBI_OK ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(urbi_register(&vm, r, "set_target_color", c_set_target_color) == URBI_OK ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(urbi_register(&vm, r, "log",              c_log)              == URBI_OK ? ESP_OK : ESP_FAIL);

    /* 5: load baked bytecode.  Panic on failure -> clean coredump
     * rather than a silent boot loop. */
    char errbuf[128] = {0};
    struct UModule *m = urbi_module_from_bytes(eye_demo_bytecode,
                                               eye_demo_bytecode_size,
                                               errbuf, sizeof errbuf);
    if (m == NULL) {
        ESP_LOGE(TAG, "urbi_module_from_bytes failed: %s", errbuf);
        ESP_ERROR_CHECK(ESP_FAIL);
    }
    int load_rc = urbi_load_module(&vm, m, "eye_demo");
    if (load_rc != URBI_OK) {
        urbi_error_info_t info = {0};
        urbi_last_error(&vm, &info);
        ESP_LOGE(TAG, "urbi_load_module failed: rc=%d code=%d line=%d ctx=%s msg=%s",
                 load_rc, info.code, info.source_line,
                 (info.context && info.context[0]) ? info.context : "(none)",
                 (info.message && info.message[0]) ? info.message : "(none)");
        ESP_ERROR_CHECK(ESP_FAIL);
    }

    /* 6: peripherals.  Display init BEFORE camera so the frame queue
     * exists by the time the camera task's first DQBUF + post_frame
     * fires (camera task is pinned to core 0 at the same priority as
     * main_task; FreeRTOS would otherwise round-robin and the camera
     * task could post a frame before display_init returned). */
    eye_display_init();
    eye_camera_init(&vm, ev_blob);
    button_install_isr(&vm, ev_btn);

    /* 7: spawn the urbi task pinned to core 0, priority +2 (above the
     * +1 camera and display tasks so urbi drains events ahead of new
     * captures). */
    urbi_task_handle = xTaskCreateStaticPinnedToCore(
        port_urbi_task_body, "urbi", URBI_STACK_WORDS, &vm,
        tskIDLE_PRIORITY + 2, urbi_stack_buf, &urbi_tcb, 0);
    ESP_ERROR_CHECK(urbi_task_handle != NULL ? ESP_OK : ESP_FAIL);

    /* 8: spawn the live-stats task on core 1 (alongside the display task)
     * — periodic dump of camera FPS, blob handler FPS, current color,
     * VM strand/watcher pool occupancy, ring overflow, and heap usage.
     * Pinned to core 1 to keep it off the urbi task's core 0 so its
     * vTaskDelay + ESP_LOG don't compete with urbi_step throughput. */
    static StackType_t  stats_stack_buf[3 * 1024 / sizeof(StackType_t)];
    static StaticTask_t stats_tcb;
    xTaskCreateStaticPinnedToCore(
        port_stats_task, "stats", 3 * 1024 / sizeof(StackType_t), &vm,
        tskIDLE_PRIORITY + 1, stats_stack_buf, &stats_tcb, 1);

    ESP_LOGI(TAG, "boot complete");
}
