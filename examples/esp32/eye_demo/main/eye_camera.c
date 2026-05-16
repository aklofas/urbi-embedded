/* SPDX-License-Identifier: BSD-3-Clause */
/* eye_camera — OV2640 capture + blob detection driver.
 *
 * Hardware: ESP32-S3-EYE v2.2.  QVGA (320x240) RGB565 in PSRAM via the
 * 2-buffer ring; camera capture overlaps blob detection thanks to
 * CAMERA_GRAB_LATEST.
 *
 * This TU is target-only — the host unit test for detect_blob lives in
 * tests/unit/test_detect_blob.c and includes detect_blob.h directly. */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"

#include "esp_camera.h"

#include "urbi/urbi.h"

#include "eye_camera.h"
#include "detect_blob.h"

/* === Camera config — ESP32-S3-EYE v2.2 pin map (see board schematic) === */
static const camera_config_t cam_config = {
    .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = 15, .pin_sscb_sda = 4,
    .pin_sscb_scl = 5, .pin_d0 = 11, .pin_d1 = 9, .pin_d2 = 8, .pin_d3 = 10,
    .pin_d4 = 12, .pin_d5 = 18, .pin_d6 = 17, .pin_d7 = 16,
    .pin_vsync = 6, .pin_href = 7, .pin_pclk = 13,
    .xclk_freq_hz = 20000000, .pixel_format = PIXFORMAT_RGB565,
    .frame_size = FRAMESIZE_QVGA,
    .fb_count = 2, .grab_mode = CAMERA_GRAB_LATEST,
    .fb_location = CAMERA_FB_IN_PSRAM,
};

/* === Camera task state ===
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
 * display_post_frame is defined in eye_display.c (Phase 4).  The forward
 * declaration here keeps Phase 3 compile-clean; the link step picks up
 * the definition once display lands.
 *
 * Frame buffer ownership contract: camera_task_body hands fb to
 * display_post_frame and does NOT call esp_camera_fb_return(fb) itself.
 * The display TU TAKES OWNERSHIP: it must call esp_camera_fb_return(fb)
 * once the blit (or queue handoff) is complete, otherwise the camera
 * driver's 2-buffer pool starves and esp_camera_fb_get returns NULL. */
static struct UVM        *cam_vm;
static urbi_event_id_t    cam_ev_blob;
static rgb565_target_t    target     = { .r = 31, .g = 0, .b = 0, .tol = 4 };
static portMUX_TYPE       target_mux = portMUX_INITIALIZER_UNLOCKED;

extern void display_post_frame(camera_fb_t *fb);

static void camera_task_body(void *arg)
{
    (void)arg;
    for (;;) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        rgb565_target_t t;
        portENTER_CRITICAL(&target_mux);
        t = target;
        portEXIT_CRITICAL(&target_mux);

        blob_t b = detect_blob((const uint16_t *)fb->buf, fb->width, fb->height, t);
        if (b.area > 0) {
            urbi_event_payload_t p;
            p.u32[0] = (uint32_t)b.x;
            p.u32[1] = (uint32_t)b.y;
            p.u32[2] = (uint32_t)b.area;
            p.u32[3] = 0;
            urbi_inject_event(cam_vm, cam_ev_blob, &p, 16);
        }

        display_post_frame(fb);
    }
}

void eye_camera_init(struct UVM *vm, urbi_event_id_t ev_blob)
{
    cam_vm      = vm;
    cam_ev_blob = ev_blob;

    ESP_ERROR_CHECK(esp_camera_init(&cam_config));

    /* Static-allocated stack + TCB keep the camera task off the heap;
     * 4 KB is enough for the inline detect_blob loop (no recursion,
     * small frame).  Pinned to core 0 so it co-schedules cooperatively
     * with the urbi VM task per spec §5.1. */
    static StackType_t  cam_stack[4096 / sizeof(StackType_t)];
    static StaticTask_t cam_tcb;
    xTaskCreateStaticPinnedToCore(camera_task_body, "cam", 4096, NULL,
                                  tskIDLE_PRIORITY + 1, cam_stack, &cam_tcb, 0);
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
