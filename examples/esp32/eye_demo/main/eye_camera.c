/* SPDX-License-Identifier: BSD-3-Clause */
/* eye_camera — OV2640 capture + blob detection driver.
 *
 * Hardware: ESP32-S3-EYE v2.2.  QVGA (320x240) RGB565 in PSRAM via the
 * 2-buffer ring; camera capture overlaps blob detection thanks to
 * CAMERA_GRAB_LATEST.
 *
 * This TU is target-only — the host unit test for detect_blob lives in
 * tests/unit/test_detect_blob.c and includes detect_blob.h directly. */

#include "esp_camera.h"

#include "eye_camera.h"

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
