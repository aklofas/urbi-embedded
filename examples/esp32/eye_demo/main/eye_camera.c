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

#include "urbi/urbi.h"

#include "eye_camera.h"
#include "eye_display.h"
#include "detect_blob.h"

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
/* Default target = wide RED that matches actual red objects under typical
 * lighting (camera sensor noise + AWB leak ~5-15 units into "should be
 * zero" channels).  Pure (31,0,0)/tol=4 matched almost nothing on a live
 * feed; the urbi script's button-press handler updates this to the
 * Realm.colors entry for the new zone (see eye_demo.u). */
static rgb565_target_t    target     = { .r = 24, .g = 6, .b = 6, .tol = 12 };
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
        if (ioctl(cam_fd, VIDIOC_DQBUF, &buf) != 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /* V4L2_BUF_FLAG_ERROR frames are still owned by us — we MUST
         * re-queue the buffer; we just skip blob detection/display so we
         * don't act on garbage pixels. */
        if (buf.flags & V4L2_BUF_FLAG_ERROR) {
            (void)eye_camera_qbuf((int)buf.index);
            continue;
        }

        rgb565_target_t t;
        portENTER_CRITICAL(&target_mux);
        t = target;
        portEXIT_CRITICAL(&target_mux);

        uint16_t *frame = (uint16_t *)cam_buffers[buf.index];
        blob_t b = detect_blob(frame, (int)cam_width, (int)cam_height, t);
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

        /* Hand (buf, index) to the display task; display calls
         * eye_camera_qbuf(index) when it's done with the buffer. */
        display_post_frame(frame, (int)cam_width, (int)cam_height,
                           (int)buf.index);
    }
}
#endif /* CONFIG_EYE_DEMO_ENABLE_CAMERA */

void eye_camera_init(struct UVM *vm, urbi_event_id_t ev_blob)
{
    cam_vm      = vm;
    cam_ev_blob = ev_blob;

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

/* Signature note: the host-fn type at <urbi/urbi.h>:295 is
 *
 *     int (*urbi_native_method_fn)(struct UVM *vm, UValue self,
 *                                  UValue *args, uint8_t nargs, UValue *out);
 *
 * The brainstorm spec sketch in §5.2 used a `UStrand *` / `UValue` return
 * convention that does not match the real v0.7.1 surface — corrected here
 * to the canonical urbi_native_method_fn shape so that
 *
 *     urbi_register(vm, realm, "set_target_color", c_set_target_color);
 *
 * (Gap A in spec §2.3) wires straight through with no shim. */
int c_set_target_color(struct UVM *vm, UValue self,
                       UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self;

    if (out) *out = urbi_make_nil();
    if (nargs < 4 || args == NULL) return UEXEC_OK;

    rgb565_target_t t = {
        .r   = (uint8_t)urbi_value_as_int(args[0]),
        .g   = (uint8_t)urbi_value_as_int(args[1]),
        .b   = (uint8_t)urbi_value_as_int(args[2]),
        .tol = (uint8_t)urbi_value_as_int(args[3]),
    };

    portENTER_CRITICAL(&target_mux);
    target = t;
    portEXIT_CRITICAL(&target_mux);

    return UEXEC_OK;
}
