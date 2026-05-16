/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_detect_blob.c — host-side unit test for the
 * ESP32-S3-EYE blob detector.  Drives detect_blob from
 * examples/esp32/eye_demo/main/detect_blob.h with synthetic 320x240
 * RGB565 buffers.  Compile-time the test picks up the header via the
 * per-file include flag added in the host Makefile (test_detect_blob.o
 * target-specific CPPFLAGS).  No ESP-IDF headers are involved — only
 * the pure C99 static-inline function in detect_blob.h. */

#include "utest.h"

#include <stdlib.h>
#include <string.h>

#include "detect_blob.h"

#define UTEST(name) static void name(void)

#define IMG_W 320
#define IMG_H 240

/* RGB565 helpers — pack/unpack 5-6-5 channels into the framebuffer. */
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((uint32_t)(r & 0x1F) << 11) |
                      ((uint32_t)(g & 0x3F) <<  5) |
                       (uint32_t)(b & 0x1F));
}

/* Fill the entire framebuffer with one colour. */
static void fill(uint16_t *buf, uint16_t px) {
    for (int i = 0; i < IMG_W * IMG_H; i++) buf[i] = px;
}

/* Paint a filled disk of `radius` centred on (cx, cy) with colour `px`. */
static void paint_disk(uint16_t *buf, int cx, int cy, int radius, uint16_t px) {
    for (int y = cy - radius; y <= cy + radius; y++) {
        if (y < 0 || y >= IMG_H) continue;
        for (int x = cx - radius; x <= cx + radius; x++) {
            if (x < 0 || x >= IMG_W) continue;
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= radius * radius) {
                buf[y * IMG_W + x] = px;
            }
        }
    }
}

/* Test 1: red disk of radius 20 centred at (160, 120) on a black field.
 * Expect centroid within ±5 of (160, 120) and area within ±15% of π·20² ≈ 1256. */
UTEST(detect_blob_red_disk_centred) {
    static uint16_t buf[IMG_W * IMG_H];
    fill(buf, rgb565(0, 0, 0));                      /* black field */
    paint_disk(buf, 160, 120, 20, rgb565(31, 0, 0)); /* pure red, 5 bits */

    rgb565_target_t t = { .r = 31, .g = 0, .b = 0, .tol = 4 };
    blob_t b = detect_blob(buf, IMG_W, IMG_H, t);

    UASSERT(b.area > 1067);  /* π·20² - 15% */
    UASSERT(b.area < 1445);  /* π·20² + 15% */
    UASSERT(b.x >= 155);
    UASSERT(b.x <= 165);
    UASSERT(b.y >= 115);
    UASSERT(b.y <= 125);
}

/* Test 2: no matching pixels — pure black field, looking for red.
 * The tol=4 window around (31, 0, 0) does NOT match (0, 0, 0). */
UTEST(detect_blob_no_match) {
    static uint16_t buf[IMG_W * IMG_H];
    fill(buf, rgb565(0, 0, 0));

    rgb565_target_t t = { .r = 31, .g = 0, .b = 0, .tol = 4 };
    blob_t b = detect_blob(buf, IMG_W, IMG_H, t);

    UASSERT_EQ(b.area, 0);
    UASSERT_EQ(b.x, 0);
    UASSERT_EQ(b.y, 0);
}

/* Test 3: two red disks at opposite corners — the detector returns a
 * single centroid of ALL matching pixels.  Disks at (80, 60) and (240,
 * 180), each radius 10.  Joint centroid is (160, 120). */
UTEST(detect_blob_multiple_blobs_joint_centroid) {
    static uint16_t buf[IMG_W * IMG_H];
    fill(buf, rgb565(0, 0, 0));
    paint_disk(buf,  80,  60, 10, rgb565(31, 0, 0));
    paint_disk(buf, 240, 180, 10, rgb565(31, 0, 0));

    rgb565_target_t t = { .r = 31, .g = 0, .b = 0, .tol = 4 };
    blob_t b = detect_blob(buf, IMG_W, IMG_H, t);

    /* Both disks have the same area, so the joint centroid is the
     * midpoint of the two centres: ((80+240)/2, (60+180)/2) = (160, 120). */
    UASSERT(b.area > 540);   /* 2 × π·10² - margin */
    UASSERT(b.area < 750);   /* 2 × π·10² + margin */
    UASSERT(b.x >= 155);
    UASSERT(b.x <= 165);
    UASSERT(b.y >= 115);
    UASSERT(b.y <= 125);
}

void test_detect_blob_suite(void) {
    utest_run("detect_blob_red_disk_centred",   detect_blob_red_disk_centred);
    utest_run("detect_blob_no_match",           detect_blob_no_match);
    utest_run("detect_blob_multiple_blobs_joint_centroid",
              detect_blob_multiple_blobs_joint_centroid);
}
