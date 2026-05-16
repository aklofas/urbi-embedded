/* SPDX-License-Identifier: BSD-3-Clause */
/* eye_camera — OV2640 capture + blob detection driver via esp_video V4L2.
 *
 * Hardware: ESP32-S3-EYE v2.2.  240x240 RGB565(LE) in PSRAM via the
 * 2-buffer mmap'd ring; camera capture overlaps blob detection because
 * the display task returns a buffer (VIDIOC_QBUF) only after the blit
 * completes.
 *
 * Migration note: this driver was originally written against the
 * espressif/esp32-camera component (esp_camera_init / esp_camera_fb_get /
 * esp_camera_fb_return).  On ESP-IDF v6.0.1 esp32-camera 2.1.6 crashes
 * inside intr_alloc.c during SCCB_Init -> i2c_new_master_bus on real
 * S3-EYE hardware (LoadProhibited, find_desc_for_source).  Espressif's
 * own ESP32-S3-EYE BSP migrated off esp32-camera onto esp_video in
 * v6.0.0 for the same reason; this TU follows that migration.  The
 * blob-detection + urbi event-injection topology is unchanged; only the
 * capture path and the camera-to-display buffer handoff have moved from
 * the camera_fb_t* model to V4L2 DQBUF/QBUF + a (buf-ptr, index) pair.
 *
 * This TU is target-only — the host unit test for detect_blob lives in
 * tests/unit/test_detect_blob.c and includes detect_blob.h directly. */

#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "esp_video_device.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"   /* heap_caps_malloc — snap buffer PSRAM alloc */
#include "esp_timer.h"       /* esp_timer_get_time — DQBUF wait histogram */

#include "urbi/urbi.h"

#include "eye_camera.h"
#include "eye_display.h"
#include "detect_blob.h"          /* legacy RGB-distance detector + blob_t */
#include "detect_blob_chroma.h"   /* chrominance-based detector (2026-05-16) */

static const char *TAG = "eye_camera";

#if CONFIG_EYE_DEMO_ENABLE_CAMERA
/* === esp_video DVP config — ESP32-S3-EYE v2.2 pin map (see board schematic) ===
 *
 * Pin map mirrors the legacy esp32-camera camera_config_t exactly: XCLK
 * GPIO15, SCCB SDA/SCL GPIO4/GPIO5, D0..D7 on GPIO11/9/8/10/12/18/17/16,
 * VSYNC GPIO6, HREF (DVP "de") GPIO7, PCLK GPIO13.  PWDN/RESET are not
 * wired on the v2.2 board so both pins are -1.  XCLK at 20 MHz matches
 * the 25fps OV2640 timing budget the Kconfig requests. */
static const esp_video_init_dvp_config_t s_dvp_config = {
    .sccb_config = {
        .init_sccb  = true,
        .i2c_config = {
            .port    = 0,
            .scl_pin = 5,
            .sda_pin = 4,
        },
        .freq = 100000,
    },
    .reset_pin = -1,
    .pwdn_pin  = -1,
    .dvp_pin = {
        .data_width = CAM_CTLR_DATA_WIDTH_8,
        .data_io    = { 11, 9, 8, 10, 12, 18, 17, 16 },
        .vsync_io   = 6,
        .de_io      = 7,    /* HREF on the OV2640 — DVP "data enable" */
        .pclk_io    = 13,
        .xclk_io    = 15,
    },
    .xclk_freq = 20000000,
};

static const esp_video_init_config_t s_video_config = {
    .dvp = &s_dvp_config,
};
#endif /* CONFIG_EYE_DEMO_ENABLE_CAMERA */

/* === Camera + V4L2 state ===
 *
 * cam_vm / cam_ev_blob are stashed by eye_camera_init and read by the
 * camera task body.  They are written once and never mutated after the
 * task starts, so no synchronisation is required.
 *
 * target / target_mux protect the active target colour against concurrent
 * updates from c_set_target_color (called on the urbi VM thread).  A
 * portMUX is lighter than a FreeRTOS mutex and is the right primitive
 * for short critical sections that never block.
 *
 * cam_fd is the V4L2 fd for /dev/video2 (ESP_VIDEO_DVP_DEVICE_NAME).
 * cam_buffers[]/cam_buffer_sizes[] hold the mmap'd frame buffers.
 * eye_camera_qbuf reads cam_fd to hand a buffer back after blit.
 *
 * Frame buffer ownership contract: camera_task_body hands (buf, index)
 * to display_post_frame and does NOT VIDIOC_QBUF the buffer itself.
 * The display TU TAKES OWNERSHIP: it must call eye_camera_qbuf(index)
 * once the blit completes, otherwise the camera driver's 2-buffer pool
 * starves and VIDIOC_DQBUF blocks forever on the camera task. */
#define BUFFER_COUNT 2

static struct UVM        *cam_vm;
static urbi_event_id_t    cam_ev_blob;
static urbi_event_id_t    cam_ev_scan_tick;

/* === urbiscript snap buffer ============================================
 *
 * 115 KB PSRAM buffer holding the most-recent fully-captured frame for
 * urbiscript-side reads (via c_get_pixel_{r,g,b}).  Camera task memcpy's
 * into this AFTER detect_blob, then injects ev_scan_tick — urbiscript's
 * BlobScan handler reads bytes here while the camera task moves on to
 * the next DQBUF.  No locking needed: the camera task's write to snap_buf
 * + scan_tick inject pair atomically transfers ownership for the
 * inter-tick interval.  If urbi falls behind (scan still in progress when
 * next tick fires), the SPSC event ring overflows and ring_ovf increments
 * — observable, not silent.
 *
 * Subsample factor in BlobScan.scan() controls pixel-read load: at the
 * default subsample=4 the urbiscript loop reads 60x60=3,600 pixels per
 * scan (~0.2s on LX7@240MHz).  Crank to subsample=1 for 240x240=57,600
 * pixels (~1.2s) to find the runtime's saturation point. */
#define SCAN_SNAP_W                  240
#define SCAN_SNAP_H                  240
#define SCAN_SNAP_BYTES              ((size_t)SCAN_SNAP_W * SCAN_SNAP_H * 2U)
#define SCAN_TICK_EVERY_N_FRAMES     50    /* @ 25fps capture = 0.5 Hz scan */

static uint16_t          *cam_snap_buf;     /* PSRAM, SCAN_SNAP_BYTES */

/* In-progress guard: when set (by c_scan_begin from urbiscript), the
 * camera task skips memcpy + scan_tick inject.  Cleared by c_scan_end at
 * end of BlobScan.scan().  Single-producer (camera task reads, urbi task
 * writes via host fns) + word-sized volatile = atomic on LX7 — no mux.
 *
 * This lets the camera task self-throttle: subsample=1 in urbiscript
 * takes several seconds per scan; without the guard, scan_ticks would
 * pile up in the SPSC event ring at the 0.5 Hz cadence and overflow.
 * With the guard, the next inject only fires AFTER urbi finishes the
 * previous scan, so the effective rate adapts to whatever urbiscript
 * can actually sustain (= the true urbiscript throughput limit). */
static volatile int       cam_scan_in_progress = 0;

/* === Diagnostic telemetry (added 2026-05-16) =========================
 *
 * Periodic per-color probe + frame-center RGB sample + DQBUF wait
 * histogram.  Updated by camera_task_body at PROBE_EVERY_N_FRAMES
 * cadence (0.5 Hz @ 25fps).  Reads via c_probe_{red,green,blue}() /
 * c_center_{r,g,b}() / c_dqbuf_max_wait_us() host fns.  All single-
 * producer (camera task) + word-sized volatile = atomic on LX7. */
#define PROBE_EVERY_N_FRAMES 50

/* Hardcoded probe targets — MUST match the Realm.colors entries in
 * eye_demo.u (RED / GREEN / BLUE).  Out-of-sync = misleading telemetry,
 * not a runtime bug; review both sites when tuning.  Chrominance-based
 * (see detect_blob_chroma.h): dominant channel + dominance + min_bright. */
/* Probe thresholds mirror the Realm.colors per-channel tuning in
 * eye_demo.u: GREEN uses a more permissive (2/6) since OV2640 AWB
 * actively suppresses green saturation; RED/BLUE use (4/8). */
static const chroma_target_t probe_targets[3] = {
    { .dominant = CHROMA_R, .dominance = 4, .min_bright = 8 },  /* RED   */
    { .dominant = CHROMA_G, .dominance = 2, .min_bright = 6 },  /* GREEN */
    { .dominant = CHROMA_B, .dominance = 4, .min_bright = 8 },  /* BLUE  */
};

static volatile uint32_t  g_probe_hits_red   = 0;
static volatile uint32_t  g_probe_hits_green = 0;
static volatile uint32_t  g_probe_hits_blue  = 0;
static volatile uint32_t  g_center_r         = 0;  /* 0..31 (5-bit) */
static volatile uint32_t  g_center_g         = 0;  /* 0..63 (6-bit) */
static volatile uint32_t  g_center_b         = 0;  /* 0..31 (5-bit) */
static volatile uint32_t  g_dqbuf_max_wait_us = 0; /* reset on read   */
/* Default target = RED-dominant.  Chrominance-based: a pixel matches
 * when the dominant channel is ≥ dominance brighter than the other two
 * AND ≥ min_bright (gates noise in shadows).  See detect_blob_chroma.h.
 * The urbi script's button-press handler updates this to the
 * Realm.colors[Realm.idx] entry for the new zone (see eye_demo.u). */
static chroma_target_t    target     = { .dominant = CHROMA_R,
                                         .dominance = 4,
                                         .min_bright = 8 };
static portMUX_TYPE       target_mux = portMUX_INITIALIZER_UNLOCKED;

static int                cam_fd = -1;
#if CONFIG_EYE_DEMO_ENABLE_CAMERA
static uint8_t           *cam_buffers[BUFFER_COUNT];
static uint32_t           cam_buffer_sizes[BUFFER_COUNT];
static uint32_t           cam_width;
static uint32_t           cam_height;
#endif

int eye_camera_qbuf(int qbuf_index)
{
    if (cam_fd < 0 || qbuf_index < 0 || qbuf_index >= BUFFER_COUNT) {
        return -1;
    }
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = (uint32_t)qbuf_index;
    if (ioctl(cam_fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGW(TAG, "VIDIOC_QBUF idx=%d failed", qbuf_index);
        return -1;
    }
    return 0;
}

#if CONFIG_EYE_DEMO_ENABLE_CAMERA
static void camera_task_body(void *arg)
{
    (void)arg;
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (;;) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = type;
        buf.memory = V4L2_MEMORY_MMAP;
        int64_t dqbuf_t0 = esp_timer_get_time();
        /* DQBUF-failure streak counter — visible alive-but-failing signal.
         * On 2026-05-16 the camera task silently stopped producing frames
         * after ~45s (cam=0 in stats with no log); the user observed the
         * LCD freeze.  This log discriminates "DQBUF still being called
         * but failing" (sensor / V4L2 issue) from "DQBUF never called"
         * (camera task crashed / hung elsewhere). */
        static int dqbuf_err_streak = 0;
        if (ioctl(cam_fd, VIDIOC_DQBUF, &buf) != 0) {
            if ((++dqbuf_err_streak % 100) == 0) {
                ESP_LOGW(TAG, "VIDIOC_DQBUF failed %d times consecutively",
                         dqbuf_err_streak);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        dqbuf_err_streak = 0;
        /* DQBUF wait histogram (max-since-last-read).  Spike here means
         * camera-task starvation; idle steady-state is ~40ms (one frame). */
        int64_t dqbuf_wait = esp_timer_get_time() - dqbuf_t0;
        if ((uint32_t)dqbuf_wait > g_dqbuf_max_wait_us) {
            g_dqbuf_max_wait_us = (uint32_t)dqbuf_wait;
        }

        /* V4L2_BUF_FLAG_ERROR frames are still owned by us — we MUST
         * re-queue the buffer; we just skip blob detection/display so we
         * don't act on garbage pixels. */
        if (buf.flags & V4L2_BUF_FLAG_ERROR) {
            (void)eye_camera_qbuf((int)buf.index);
            continue;
        }

        chroma_target_t t;
        portENTER_CRITICAL(&target_mux);
        t = target;
        portEXIT_CRITICAL(&target_mux);

        uint16_t *frame = (uint16_t *)cam_buffers[buf.index];
        blob_t b = detect_blob_chroma(frame, (int)cam_width, (int)cam_height, t);
        /* Stats counters read by port_stats_task in eye_demo_main.c. */
        extern volatile uint32_t g_cam_frames;
        extern volatile uint32_t g_cam_injects;
        g_cam_frames++;
        if (b.area > 0) {
            urbi_event_payload_t p;
            p.u32[0] = (uint32_t)b.x;
            p.u32[1] = (uint32_t)b.y;
            p.u32[2] = (uint32_t)b.area;
            p.u32[3] = 0;
            urbi_inject_event(cam_vm, cam_ev_blob, &p, 16);
            g_cam_injects++;
        }

        /* Every PROBE_EVERY_N_FRAMES: run detect_blob against ALL three
         * probe targets + sample frame-center RGB.  Diagnostic only —
         * results feed c_probe_{red,green,blue}() and c_center_{r,g,b}()
         * host fns.  Cost: 3 × ~2 ms per probe at 0.5 Hz = 0.12 % CPU. */
        if ((g_cam_frames % PROBE_EVERY_N_FRAMES) == 0U) {
            g_probe_hits_red   = (uint32_t)detect_blob_chroma(frame, (int)cam_width,
                                  (int)cam_height, probe_targets[0]).area;
            g_probe_hits_green = (uint32_t)detect_blob_chroma(frame, (int)cam_width,
                                  (int)cam_height, probe_targets[1]).area;
            g_probe_hits_blue  = (uint32_t)detect_blob_chroma(frame, (int)cam_width,
                                  (int)cam_height, probe_targets[2]).area;
            uint16_t center_px = frame[(size_t)(cam_height/2U) * cam_width
                                        + (size_t)(cam_width/2U)];
            g_center_r = (uint32_t)((center_px >> 11) & 0x1F);
            g_center_g = (uint32_t)((center_px >>  5) & 0x3F);
            g_center_b = (uint32_t)( center_px        & 0x1F);
        }

        /* Every Nth frame: if no urbi-scan is currently running, snapshot
         * the frame to cam_snap_buf and fire scan_tick so the urbiscript
         * BlobScan handler can read pixels.  The cam_scan_in_progress
         * guard (set by c_scan_begin, cleared by c_scan_end) prevents
         * scan_tick from queuing while a previous scan is mid-flight —
         * effective scan rate adapts to urbiscript throughput, not the
         * fixed camera cadence.  Buffer is private to the urbiscript
         * path; V4L2 reuses `frame` on its own schedule. */
        if (cam_snap_buf != NULL && cam_ev_scan_tick != 0U
            && cam_scan_in_progress == 0
            && (g_cam_frames % SCAN_TICK_EVERY_N_FRAMES) == 0U) {
            memcpy(cam_snap_buf, frame, SCAN_SNAP_BYTES);
            extern volatile uint32_t g_evt_scan_count;
            g_evt_scan_count++;
            urbi_inject_event(cam_vm, cam_ev_scan_tick, NULL, 0U);
        }

        /* Hand (buf, index) to the display task; display calls
         * eye_camera_qbuf(index) when it's done with the buffer. */
        display_post_frame(frame, (int)cam_width, (int)cam_height,
                           (int)buf.index);
    }
}
#endif /* CONFIG_EYE_DEMO_ENABLE_CAMERA */

void eye_camera_init(struct UVM *vm,
                     urbi_event_id_t ev_blob,
                     urbi_event_id_t ev_scan_tick)
{
    cam_vm           = vm;
    cam_ev_blob      = ev_blob;
    cam_ev_scan_tick = ev_scan_tick;

    /* Allocate snap buffer in PSRAM (115 KB).  Camera task memcpy's into
     * here every SCAN_TICK_EVERY_N_FRAMES frames; urbiscript reads via
     * the c_get_pixel_{r,g,b} host fns.  If allocation fails, scan_tick
     * is simply never fired — eye_demo runs in C-side-only mode. */
    cam_snap_buf = heap_caps_malloc(SCAN_SNAP_BYTES,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (cam_snap_buf == NULL) {
        ESP_LOGW(TAG, "snap buffer alloc failed (%u bytes PSRAM); "
                       "urbiscript BlobScan path disabled",
                 (unsigned)SCAN_SNAP_BYTES);
    }

#if !CONFIG_EYE_DEMO_ENABLE_CAMERA
    /* Camera path disabled via Kconfig.  This board (or this ESP-IDF
     * release) is on the affected-silicon list documented in
     * docs/urbi-embedded-design-risks.md row "S3 + IDF v6 i2c intr_alloc
     * incompatibility": esp_video / esp32-camera / any caller of
     * i2c_new_master_bus crashes inside esp_intr_alloc_intrstatus →
     * find_desc_for_source at intr_alloc.c:192 (LoadProhibited).
     *
     * Returning early here keeps the rest of the demo (display, button,
     * reactive at-watcher, urbi VM ticks) fully exercised on hardware
     * — only the blob-detection event injection is silent. */
    ESP_LOGW(TAG, "camera disabled (CONFIG_EYE_DEMO_ENABLE_CAMERA=n)");
    ESP_LOGW(TAG, "see docs/urbi-embedded-design-risks.md for context");
    return;
#else
    /* Diagnostic checkpoint — the previous v0.7.2 attempt on real S3-EYE
     * hardware crashed inside esp_camera_init -> SCCB_Init -> i2c_new_master_bus
     * when esp32-camera 2.1.6 was built against ESP-IDF v6.0.1 (LoadProhibited
     * at intr_alloc.c:192).  We've migrated off esp32-camera onto esp_video
     * for that reason — these log lines bookend the new init so a future
     * crash here is unambiguous about which side of esp_video_init failed. */
    ESP_LOGI(TAG, "esp_video_init: starting");
    esp_err_t ret = esp_video_init(&s_video_config);
    ESP_LOGI(TAG, "esp_video_init: returned %s", esp_err_to_name(ret));
    ESP_ERROR_CHECK(ret);

    /* Open the DVP V4L2 device. */
    cam_fd = open(ESP_VIDEO_DVP_DEVICE_NAME, O_RDONLY);
    if (cam_fd < 0) {
        ESP_LOGE(TAG, "open(%s) failed", ESP_VIDEO_DVP_DEVICE_NAME);
        abort();
    }

    /* Set capture format: 240x240 RGB565(LE).  esp_cam_sensor's OV2640
     * driver enforces this via the CONFIG_CAMERA_OV2640_DVP_*_RGB565_*
     * Kconfig, but we still call VIDIOC_S_FMT to lock it in explicitly
     * and to populate cam_width/cam_height for the task body. */
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.width       = 240,
        .fmt.pix.height      = 240,
        .fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565,
    };
    if (ioctl(cam_fd, VIDIOC_S_FMT, &format) != 0) {
        ESP_LOGE(TAG, "VIDIOC_S_FMT failed");
        abort();
    }
    cam_width  = format.fmt.pix.width;
    cam_height = format.fmt.pix.height;
    ESP_LOGI(TAG, "format locked: %ux%u, fourcc=0x%08x",
             (unsigned)cam_width, (unsigned)cam_height,
             (unsigned)format.fmt.pix.pixelformat);

    /* Request 2 mmap'd buffers — matches the previous fb_count=2 ring. */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count  = BUFFER_COUNT;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(cam_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
        abort();
    }

    /* Query + mmap each buffer, then queue it. */
    for (int i = 0; i < BUFFER_COUNT; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = (uint32_t)i;
        if (ioctl(cam_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF idx=%d failed", i);
            abort();
        }
        cam_buffers[i] = (uint8_t *)mmap(NULL, buf.length,
                                         PROT_READ | PROT_WRITE,
                                         MAP_SHARED, cam_fd, buf.m.offset);
        if (cam_buffers[i] == MAP_FAILED) {
            ESP_LOGE(TAG, "mmap idx=%d failed", i);
            abort();
        }
        cam_buffer_sizes[i] = buf.length;
        if (ioctl(cam_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF idx=%d failed", i);
            abort();
        }
    }

    /* Start streaming. */
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        abort();
    }
    ESP_LOGI(TAG, "stream started (%u buffers @ %u bytes)",
             (unsigned)BUFFER_COUNT, (unsigned)cam_buffer_sizes[0]);

    /* Static-allocated stack + TCB keep the camera task off the heap;
     * 4 KB is enough for the inline detect_blob loop (no recursion,
     * small frame).  Pinned to core 0 so it co-schedules cooperatively
     * with the urbi VM task per spec §5.1. */
    static StackType_t  cam_stack[4096 / sizeof(StackType_t)];
    static StaticTask_t cam_tcb;
    xTaskCreateStaticPinnedToCore(camera_task_body, "cam", 4096, NULL,
                                  tskIDLE_PRIORITY + 1, cam_stack, &cam_tcb, 0);
#endif /* CONFIG_EYE_DEMO_ENABLE_CAMERA */
}

/* Host fn — push a chrominance target from urbiscript to the camera task.
 *
 *     set_target_chroma(dominant, dominance, min_bright)
 *
 *   dominant   — 0 / 1 / 2 (CHROMA_R / CHROMA_G / CHROMA_B; see header)
 *   dominance  — min channel-dominance margin (5-bit scale; typ 6-12)
 *   min_bright — gate noise in dark regions (5-bit scale; typ 10-16)
 *
 * Replaces the legacy c_set_target_color (RGB-distance) — see
 * detect_blob_chroma.h for the rationale (GREEN tracking was unusable
 * with RGB-distance under AWB).  Signature matches the canonical
 * urbi_native_method_fn from <urbi/urbi.h>:295. */
int c_set_target_chroma(struct UVM *vm, UValue self,
                        UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self;

    if (out) *out = urbi_make_nil();
    if (nargs < 3 || args == NULL) return UEXEC_OK;

    chroma_target_t t = {
        .dominant   = (uint8_t)urbi_value_as_int(args[0]),
        .dominance  = (uint8_t)urbi_value_as_int(args[1]),
        .min_bright = (uint8_t)urbi_value_as_int(args[2]),
    };

    portENTER_CRITICAL(&target_mux);
    target = t;
    portEXIT_CRITICAL(&target_mux);

    return UEXEC_OK;
}

/* === Snap-buffer pixel reads ============================================
 *
 * urbiscript reads channel values out of the camera task's snapshot via
 * these three host fns.  Args are (x, y) integers; out-of-range returns
 * 0 silently (a 240x240 BlobScan loop runs the bounds check itself, so
 * the silent clamp is just a final safety net).
 *
 * Performance hot path: the urbiscript BlobScan inner loop calls these
 * three fns per pixel (3 dispatches + 6 arithmetic ops at minimum).  Per-
 * frame load at subsample=4: 60x60=3,600 pixels × 3 = 10,800 host fn
 * calls per scan tick.  Each fn body is ~10 instructions so the C work
 * itself is tiny — most of the cost lives in the urbiscript bytecode
 * dispatch, which is the point of this stress test. */

#define SNAP_PIXEL_GUARD()                                                 \
    do {                                                                   \
        if (out) *out = urbi_make_int(0);                                  \
        if (cam_snap_buf == NULL) return UEXEC_OK;                         \
        if (nargs < 2 || args == NULL) return UEXEC_OK;                    \
    } while (0)

static inline uint16_t snap_pixel_at(int x, int y)
{
    if (x < 0 || x >= SCAN_SNAP_W || y < 0 || y >= SCAN_SNAP_H) return 0U;
    return cam_snap_buf[(size_t)y * SCAN_SNAP_W + (size_t)x];
}

int c_get_pixel_r(struct UVM *vm, UValue self,
                  UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self;
    SNAP_PIXEL_GUARD();
    uint16_t px = snap_pixel_at((int)urbi_value_as_int(args[0]),
                                 (int)urbi_value_as_int(args[1]));
    if (out) *out = urbi_make_int((int64_t)((px >> 11) & 0x1F));
    return UEXEC_OK;
}

int c_get_pixel_g(struct UVM *vm, UValue self,
                  UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self;
    SNAP_PIXEL_GUARD();
    uint16_t px = snap_pixel_at((int)urbi_value_as_int(args[0]),
                                 (int)urbi_value_as_int(args[1]));
    if (out) *out = urbi_make_int((int64_t)((px >> 5) & 0x3F));
    return UEXEC_OK;
}

int c_get_pixel_b(struct UVM *vm, UValue self,
                  UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self;
    SNAP_PIXEL_GUARD();
    uint16_t px = snap_pixel_at((int)urbi_value_as_int(args[0]),
                                 (int)urbi_value_as_int(args[1]));
    if (out) *out = urbi_make_int((int64_t)(px & 0x1F));
    return UEXEC_OK;
}

/* Scan-in-progress guard (option C — adaptive scan rate).  urbiscript's
 * BlobScan.scan() brackets its body in c_scan_begin / c_scan_end; the
 * camera task skips scan_tick injection while the flag is set.  Effective
 * scan cadence: max(SCAN_TICK_EVERY_N_FRAMES/25 Hz, urbi-scan-duration).
 * Subsample=1 (240x240 full grid) becomes self-throttling — the system
 * finds its own steady-state rate based on actual urbiscript throughput. */
int c_scan_begin(struct UVM *vm, UValue self,
                 UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    cam_scan_in_progress = 1;
    if (out) *out = urbi_make_nil();
    return UEXEC_OK;
}

int c_scan_end(struct UVM *vm, UValue self,
               UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    cam_scan_in_progress = 0;
    if (out) *out = urbi_make_nil();
    return UEXEC_OK;
}

/* === Diagnostic host fns ============================================ */

/* Per-color probe counts — last measured detect_blob result against the
 * 3 probe_targets[].  Updated every PROBE_EVERY_N_FRAMES; reads always
 * return the most-recent snapshot.  RED/GREEN/BLUE in lock-step. */
#define DEFINE_STAT_INT_GETTER(name, var)                                    \
int name(struct UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out) { \
    (void)vm; (void)self; (void)args; (void)nargs;                           \
    if (out) *out = urbi_make_int((int64_t)(var));                           \
    return UEXEC_OK;                                                         \
}

DEFINE_STAT_INT_GETTER(c_probe_red,   g_probe_hits_red)
DEFINE_STAT_INT_GETTER(c_probe_green, g_probe_hits_green)
DEFINE_STAT_INT_GETTER(c_probe_blue,  g_probe_hits_blue)
DEFINE_STAT_INT_GETTER(c_center_r,    g_center_r)
DEFINE_STAT_INT_GETTER(c_center_g,    g_center_g)
DEFINE_STAT_INT_GETTER(c_center_b,    g_center_b)

/* DQBUF max wait time since last read — auto-resets on read so each
 * stats interval shows that interval's max, not all-time max. */
int c_dqbuf_max_wait_us(struct UVM *vm, UValue self,
                        UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    uint32_t v = g_dqbuf_max_wait_us;
    g_dqbuf_max_wait_us = 0;
    if (out) *out = urbi_make_int((int64_t)v);
    return UEXEC_OK;
}
