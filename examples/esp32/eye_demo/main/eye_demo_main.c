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
#include "urbi/aux.h"     /* urbi_aux_register_function_table — batch stats fns */
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
 * line per call.  Truncates at 320 bytes; covers the wide Stats.snapshot
 * diagnostic line (~22 fields).
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

    /* 320-byte buffer — was 160, hit truncation in eye_demo.u Stats.snapshot
     * once the diagnostic field set grew to ~22 fields per line (2026-05-16).
     * Bumped again to 512 when the Tier-2 at-sync stress addition pushed the
     * line past 320 (visible as mid-field truncation on `sync=N`).
     * Stack-allocated; no runtime cost beyond an extra ~352 B on the urbi
     * task stack while c_log runs.  Embedders that don't dump such wide
     * diagnostic lines can shrink back to 160 with no functional change. */
    char buf[512];
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

/* Latest-tick FPS deltas, computed by port_stats_task and read by the
 * c_cam_fps / c_inject_fps host fns from the urbi task.  Single producer
 * (stats task) + single consumer (urbi task); volatile + 32-bit-aligned
 * load/store on Xtensa is atomic, no further sync needed. */
static volatile uint32_t g_last_cam_fps    = 0;
static volatile uint32_t g_last_inject_fps = 0;

/* Heap min tracking: lowest observed free-bytes since last reset.  Reset
 * on read (each c_heap_X_min_kb call) so successive stats lines show
 * the per-interval low-water mark, not all-time min. */
static volatile uint32_t g_heap_int_min_bytes   = UINT32_MAX;
static volatile uint32_t g_heap_psram_min_bytes = UINT32_MAX;

/* Event-ring high-water mark since last read.  Sampled at end of each
 * urbi_step inside port_urbi_task_body (after drain). */
static volatile uint32_t g_ring_max_seen = 0;

/* Step-result distribution: counts of URBI_STEP_RUNNING / QUIESCENT /
 * WAKE_AT / FATAL since port boot.  urbiscript reads via c_step_X(). */
static volatile uint32_t g_step_running    = 0;
static volatile uint32_t g_step_quiescent  = 0;
static volatile uint32_t g_step_wake_at    = 0;
static volatile uint32_t g_step_fatal      = 0;

/* Per-event injection counters (camera-task is producer for blob_seen;
 * button ISR for button_pressed; port_stats_task for stats_tick &
 * scan_tick).  Symmetric to g_cam_injects for blob_seen — the others
 * grow per inject site.  Read deltas via c_evt_X().
 *
 * btn + scan counters are EXTERN (no `static`) so eye_button.c and
 * eye_camera.c can bump them from the inject sites without owning the
 * underlying storage.  stats counter is internal — incremented locally
 * in port_stats_task. */
volatile uint32_t g_evt_btn_count   = 0;
static volatile uint32_t g_evt_stats_count = 0;
volatile uint32_t g_evt_scan_count  = 0;

/* stats_tick event id — fired from port_stats_task every STATS_PERIOD_MS
 * to drive the urbiscript-side Stats class.  Stashed at registration. */
static urbi_event_id_t   g_stats_tick_ev = 0;

/* === Host fns: stats accessors ====================================
 *
 * Each fn is called from a urbi-task at-handler body, so they run in
 * MAIN script context — safe to touch vm-> fields directly without
 * worrying about cross-task ordering (the stats task only writes
 * g_last_*_fps and the event ring, both single-producer / single-
 * consumer aligned-32-bit writes). */
static int c_cam_fps(struct UVM *vm, UValue self,
                     UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    if (out != NULL) *out = urbi_make_int((int64_t)g_last_cam_fps);
    return UEXEC_OK;
}

static int c_inject_fps(struct UVM *vm, UValue self,
                        UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    if (out != NULL) *out = urbi_make_int((int64_t)g_last_inject_fps);
    return UEXEC_OK;
}

static int c_strands(struct UVM *vm, UValue self,
                     UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)args; (void)nargs;
    if (out != NULL) *out = urbi_make_int((int64_t)vm->strand_runnable_count);
    return UEXEC_OK;
}

static int c_watchers_in_use(struct UVM *vm, UValue self,
                              UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)args; (void)nargs;
    if (out != NULL) *out = urbi_make_int((int64_t)vm->watcher_pool_in_use);
    return UEXEC_OK;
}

static int c_watchers_high_water(struct UVM *vm, UValue self,
                                  UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)args; (void)nargs;
    if (out != NULL) *out = urbi_make_int((int64_t)vm->watcher_pool_high_water);
    return UEXEC_OK;
}

static int c_ring_ovf(struct UVM *vm, UValue self,
                      UValue *args, uint8_t nargs, UValue *out)
{
    (void)self; (void)args; (void)nargs;
    UEventRing *ring = vm->event_ring;
    if (out != NULL) {
        *out = urbi_make_int((int64_t)(ring ? ring->overflow_count : 0U));
    }
    return UEXEC_OK;
}

static int c_heap_int_kb(struct UVM *vm, UValue self,
                         UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (out != NULL) *out = urbi_make_int((int64_t)(free_bytes / 1024U));
    return UEXEC_OK;
}

static int c_heap_psram_kb(struct UVM *vm, UValue self,
                           UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (out != NULL) *out = urbi_make_int((int64_t)(free_bytes / 1024U));
    return UEXEC_OK;
}

/* === Diagnostic host fns: step-result distribution, ring max, heap min,
 * per-event injection counters.  All boilerplate getters; reads of
 * "since-last-read" stats auto-reset so successive samples show
 * per-interval deltas, not all-time totals. */
#define STAT_RESET_ON_READ(name, var)                                        \
static int name(struct UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out) { \
    (void)vm; (void)self; (void)args; (void)nargs;                           \
    uint32_t v = (var); (var) = 0;                                           \
    if (out) *out = urbi_make_int((int64_t)v);                               \
    return UEXEC_OK;                                                         \
}
#define STAT_GETTER(name, var)                                               \
static int name(struct UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out) { \
    (void)vm; (void)self; (void)args; (void)nargs;                           \
    if (out) *out = urbi_make_int((int64_t)(var));                           \
    return UEXEC_OK;                                                         \
}

STAT_RESET_ON_READ(c_step_running,    g_step_running)
STAT_RESET_ON_READ(c_step_quiescent,  g_step_quiescent)
STAT_RESET_ON_READ(c_step_wake_at,    g_step_wake_at)
STAT_GETTER(       c_step_fatal,      g_step_fatal)
STAT_RESET_ON_READ(c_ring_max,        g_ring_max_seen)
STAT_RESET_ON_READ(c_evt_btn,         g_evt_btn_count)
STAT_RESET_ON_READ(c_evt_stats,       g_evt_stats_count)
STAT_RESET_ON_READ(c_evt_scan,        g_evt_scan_count)

/* Forward-decl of the static vm (defined later in this TU as the app_main
 * singleton).  Needed because port_urbi_step_observed below samples
 * vm.event_ring on every step. */
static struct UVM vm;

/* Strong override of the port's weak `port_urbi_step_observed` hook —
 * bumps per-result counters + samples the event-ring high-water mark
 * each step.  port_freertos_task.c calls us on every urbi_step return. */
void port_urbi_step_observed(int result_int, uint64_t wake_us)
{
    (void)wake_us;
    switch (result_int) {
        case URBI_STEP_RUNNING:    g_step_running++;    break;
        case URBI_STEP_QUIESCENT:  g_step_quiescent++;  break;
        case URBI_STEP_WAKE_AT:    g_step_wake_at++;    break;
        case URBI_STEP_FATAL:      g_step_fatal++;      break;
        default: break;
    }
    /* Sample ring high-water mark.  vm is the same singleton; pull it
     * from the file-scope vm.  Avoid the call if ring is null (pre-init). */
    /* Race-safe ring depth sample.  Both indices grow monotonically;
     * write_idx is bumped after inject, read_idx after drain.  If the
     * camera task injects between our two volatile loads (write_idx then
     * read_idx), read can lap forward and the unsigned subtract wraps to
     * 0xFFFFFFXX (saw 4294967041 in the 2026-05-16 log).  Snapshot in
     * the safer order (read first, then write) and clamp negative-by-
     * wrap to 0 — high-water sampling is approximate anyway. */
    UEventRing *ring = vm.event_ring;
    if (ring != NULL) {
        uint32_t r_idx = ring->read_idx;
        uint32_t w_idx = ring->write_idx;
        int32_t  depth = (int32_t)(w_idx - r_idx);
        if (depth > 0 && (uint32_t)depth > g_ring_max_seen) {
            g_ring_max_seen = (uint32_t)depth;
        }
    }
}

/* Heap min: report low-water-mark in KB, then reset to current free so
 * the next interval re-acquires its own low. */
static int c_heap_int_min_kb(struct UVM *vm, UValue self,
                             UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    uint32_t v = g_heap_int_min_bytes;
    g_heap_int_min_bytes = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (out) *out = urbi_make_int((int64_t)((v == UINT32_MAX ? 0 : v) / 1024U));
    return UEXEC_OK;
}
static int c_heap_psram_min_kb(struct UVM *vm, UValue self,
                               UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    uint32_t v = g_heap_psram_min_bytes;
    g_heap_psram_min_bytes = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (out) *out = urbi_make_int((int64_t)((v == UINT32_MAX ? 0 : v) / 1024U));
    return UEXEC_OK;
}

/* port_stats_task — periodic FPS-delta compute + stats_tick event inject.
 *
 * Pre-move (v0.7.2-pre-stress-test): formatted + ESP_LOGI'd the full stats
 * line directly from this task, reading vm-> fields without sync (documented
 * as MAIN-only by the v0.7.1 contract).  Stress-test refactor (2026-05-16):
 * format + log moved to urbiscript via the Stats class.  This task now only:
 *
 *   1. Computes cam/inject FPS deltas (rolling N-frame counters).
 *   2. Stores them in volatile statics (read by c_cam_fps / c_inject_fps
 *      from the urbi task).
 *   3. Injects stats_tick (urbi_inject_event is task-safe).
 *
 * The urbi-side at(stats_tick?) handler then calls Stats.snapshot() which
 * reads the 8 host-fn accessors and emits a single log() line.  This
 * exercises the cross-task event-injection + at-body class-method-invocation
 * + multi-host-fn-per-handler patterns on real hardware. */
static void port_stats_task(void *arg)
{
    struct UVM *vm = (struct UVM *)arg;

    uint32_t prev_cam     = 0;
    uint32_t prev_injects = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATS_PERIOD_MS));

        uint32_t cam = g_cam_frames;
        uint32_t inj = g_cam_injects;

        g_last_cam_fps    = (cam - prev_cam)     * 1000U / STATS_PERIOD_MS;
        g_last_inject_fps = (inj - prev_injects) * 1000U / STATS_PERIOD_MS;

        prev_cam     = cam;
        prev_injects = inj;

        /* Sample heap low-water marks (read once per stats tick — cheap
         * enough; if the embedder wants per-frame resolution, move into
         * camera_task_body).  ~4 us per call on ESP32-S3. */
        uint32_t hi_now = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        uint32_t hp_now = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        if (hi_now < g_heap_int_min_bytes)   g_heap_int_min_bytes   = hi_now;
        if (hp_now < g_heap_psram_min_bytes) g_heap_psram_min_bytes = hp_now;

        if (g_stats_tick_ev != 0U) {
            g_evt_stats_count++;
            urbi_inject_event(vm, (uint32_t)g_stats_tick_ev, NULL, 0U);
        }
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

/* (vm forward-decl is up at the top, near port_urbi_step_observed.) */
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

    urbi_event_id_t ev_blob      = urbi_event_register(&vm, r, "blob_seen",
                                                       destructure_blob, NULL);
    urbi_event_id_t ev_btn       = urbi_event_register(&vm, r, "button_pressed",
                                                       NULL, NULL);
    urbi_event_id_t ev_scan_tick = urbi_event_register(&vm, r, "scan_tick",
                                                       NULL, NULL);
    g_stats_tick_ev              = urbi_event_register(&vm, r, "stats_tick",
                                                       NULL, NULL);
    ESP_ERROR_CHECK(ev_blob         != URBI_EVENT_ID_INVALID ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(ev_btn          != URBI_EVENT_ID_INVALID ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(ev_scan_tick    != URBI_EVENT_ID_INVALID ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(g_stats_tick_ev != URBI_EVENT_ID_INVALID ? ESP_OK : ESP_FAIL);

    ESP_ERROR_CHECK(urbi_register(&vm, r, "draw_crosshair",    c_draw_crosshair)    == URBI_OK ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(urbi_register(&vm, r, "set_target_chroma", c_set_target_chroma) == URBI_OK ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(urbi_register(&vm, r, "log",               c_log)               == URBI_OK ? ESP_OK : ESP_FAIL);

    /* Stats accessor batch — 8 single-getter host fns the urbiscript-side
     * Stats class polls inside its at(stats_tick?) handler.  Bundled via
     * urbi_aux_register_function_table; the urbi_aux sibling component
     * (components/esp32-idf-aux/) makes the convenience layer linkable from
     * ESP-IDF builds that opt in via REQUIRES esp32-idf-aux. */
    static const urbi_aux_function_decl_t stats_fns[] = {
        { "c_cam_fps",             c_cam_fps             },
        { "c_inject_fps",          c_inject_fps          },
        { "c_strands",             c_strands             },
        { "c_watchers_in_use",     c_watchers_in_use     },
        { "c_watchers_high_water", c_watchers_high_water },
        { "c_ring_ovf",            c_ring_ovf            },
        { "c_heap_int_kb",         c_heap_int_kb         },
        { "c_heap_psram_kb",       c_heap_psram_kb       },
        /* Snap-buffer pixel reads — exposed for the urbiscript BlobScan
         * stress-test handler in eye_demo.u.  Live in eye_camera.c
         * because they reach into the snap buffer the camera task owns. */
        { "c_get_pixel_r",         c_get_pixel_r         },
        { "c_get_pixel_g",         c_get_pixel_g         },
        { "c_get_pixel_b",         c_get_pixel_b         },
        /* Scan-in-progress guard (option C) — urbiscript wraps its body
         * in c_scan_begin / c_scan_end so the camera task can adapt the
         * scan_tick injection rate to actual urbiscript throughput. */
        { "c_scan_begin",          c_scan_begin          },
        { "c_scan_end",            c_scan_end            },
        /* Diagnostic telemetry (2026-05-16) — per-color blob probe,
         * frame-center RGB, DQBUF wait, step-result distribution, heap
         * low-water-marks, event-ring high-water, per-event injection
         * rates.  The Stats class polls these into its at(stats_tick?)
         * snapshot to drive the diagnostic stats log line. */
        { "c_probe_red",           c_probe_red           },
        { "c_probe_green",         c_probe_green         },
        { "c_probe_blue",          c_probe_blue          },
        { "c_center_r",            c_center_r            },
        { "c_center_g",            c_center_g            },
        { "c_center_b",            c_center_b            },
        { "c_dqbuf_max_wait_us",   c_dqbuf_max_wait_us   },
        { "c_step_running",        c_step_running        },
        { "c_step_quiescent",      c_step_quiescent      },
        { "c_step_wake_at",        c_step_wake_at        },
        { "c_step_fatal",          c_step_fatal          },
        { "c_ring_max",            c_ring_max            },
        { "c_heap_int_min_kb",     c_heap_int_min_kb     },
        { "c_heap_psram_min_kb",   c_heap_psram_min_kb   },
        { "c_evt_btn",             c_evt_btn             },
        { "c_evt_stats",           c_evt_stats           },
        { "c_evt_scan",            c_evt_scan            },
    };
    ESP_ERROR_CHECK(urbi_aux_register_function_table(
        &vm, r, stats_fns, sizeof stats_fns / sizeof stats_fns[0])
        == URBI_OK ? ESP_OK : ESP_FAIL);

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
    eye_camera_init(&vm, ev_blob, ev_scan_tick);
    button_install_isr(&vm, ev_btn);

    /* 7: spawn the urbi task pinned to core 0 at priority +1 — SAME as
     * the camera task (+1) and display task (+1).  Equal priority gives
     * FreeRTOS round-robin time-slicing via configTICK_RATE_HZ (10 ms
     * ticks by default), so a long-running urbi body strand (e.g. the
     * urbiscript BlobScan stress-test loop, ~200 ms of bytecode per
     * fire) does NOT starve the camera task on core 0.  At higher urbi
     * priority (the original +2) FreeRTOS never preempted urbi with the
     * lower-priority camera task, and a 200 ms scan window froze the
     * LCD for 200 ms every scan tick because no new frames were
     * captured during the urbi-monopoly window.
     *
     * The tradeoff: at-handler dispatch latency goes from microseconds
     * (urbi-on-top of camera+display) to up-to-one-tick (10 ms worst
     * case before urbi gets its slice back).  Acceptable for the
     * scripted demo; embedders running hard-real-time at-handlers can
     * bump urbi back to +2 and accept the camera-task starvation as
     * a documented consequence (or shrink URBI_STEP_BUDGET further). */
    urbi_task_handle = xTaskCreateStaticPinnedToCore(
        port_urbi_task_body, "urbi", URBI_STACK_WORDS, &vm,
        tskIDLE_PRIORITY + 1, urbi_stack_buf, &urbi_tcb, 0);
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
