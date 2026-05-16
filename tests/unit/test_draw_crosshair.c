/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_draw_crosshair.c — host-side unit test for the
 * ESP32-S3-EYE display crosshair overlay.  Drives draw_crosshair_into
 * from examples/esp32/eye_demo/main/crosshair.h with synthetic 240x240
 * RGB565 buffers.  Compile-time the test picks up the header via the
 * per-file include flag added in the host Makefile (test_draw_crosshair.o
 * target-specific CPPFLAGS).  No ESP-IDF headers are involved — only
 * the pure C99 static-inline function in crosshair.h. */

#include "utest.h"

#include <string.h>

#include "crosshair.h"

#define UTEST(name) static void name(void)

#define FB_W 240
#define FB_H 240

#define WHITE  0xFFFFu
#define BLACK  0x0000u

/* Test 1: crosshair at the framebuffer centre (120, 120) should light
 * exactly the 17 horizontal pixels x ∈ [112..128] on row 120, and the 17
 * vertical pixels y ∈ [112..128] on column 120.  Every other pixel in
 * the buffer must remain black. */
UTEST(draw_crosshair_centre) {
    static uint16_t fb[FB_W * FB_H];
    memset(fb, 0, sizeof fb);

    draw_crosshair_into(fb, FB_W, FB_H, 120, 120);

    /* Horizontal arm: row 120, x = 112..128 are white. */
    for (int x = 112; x <= 128; x++) {
        UASSERT(fb[120 * FB_W + x] == WHITE);
    }
    /* Vertical arm: column 120, y = 112..128 are white. */
    for (int y = 112; y <= 128; y++) {
        UASSERT(fb[y * FB_W + 120] == WHITE);
    }

    /* Outside-arm sanity: pixels just past each arm tip stay black, and
     * a couple of unrelated points stay black.  We sample rather than
     * scan the whole buffer to keep the failure mode informative. */
    UASSERT(fb[120 * FB_W + 111] == BLACK);  /* just left of left tip  */
    UASSERT(fb[120 * FB_W + 129] == BLACK);  /* just right of right tip */
    UASSERT(fb[111 * FB_W + 120] == BLACK);  /* just above top tip      */
    UASSERT(fb[129 * FB_W + 120] == BLACK);  /* just below bottom tip   */
    UASSERT(fb[0   * FB_W +   0] == BLACK);  /* top-left corner          */
    UASSERT(fb[239 * FB_W + 239] == BLACK);  /* bottom-right corner      */
    UASSERT(fb[119 * FB_W + 119] == BLACK);  /* diagonal-of-centre off arm */
}

/* Test 2: negative coordinates are the "no crosshair pending" sentinel.
 * The buffer must come back byte-identical to its pre-call state. */
UTEST(draw_crosshair_negative_noop) {
    static uint16_t fb[FB_W * FB_H];
    /* Seed with a non-trivial pattern so any spurious write shows up. */
    for (int i = 0; i < FB_W * FB_H; i++) fb[i] = (uint16_t)(i & 0xFFFF);

    static uint16_t snapshot[FB_W * FB_H];
    memcpy(snapshot, fb, sizeof fb);

    draw_crosshair_into(fb, FB_W, FB_H, -1, -1);
    UASSERT(memcmp(fb, snapshot, sizeof fb) == 0);

    /* Asymmetric sentinels (only one of the two negative) also no-op. */
    draw_crosshair_into(fb, FB_W, FB_H, -1, 120);
    UASSERT(memcmp(fb, snapshot, sizeof fb) == 0);

    draw_crosshair_into(fb, FB_W, FB_H, 120, -1);
    UASSERT(memcmp(fb, snapshot, sizeof fb) == 0);
}

/* Test 3: crosshair centred at (0, 0).  The horizontal arm's negative-dx
 * half (cx + dx = -8..-1) and the vertical arm's negative-dy half
 * (cy + dy = -8..-1) are out of bounds and clipped per-pixel.  The
 * positive-dx half (0..8) on row 0 and the positive-dy half (0..8) on
 * column 0 must be drawn.  Pixel (0, 0) is shared by both arms. */
UTEST(draw_crosshair_edge_origin) {
    static uint16_t fb[FB_W * FB_H];
    memset(fb, 0, sizeof fb);

    draw_crosshair_into(fb, FB_W, FB_H, 0, 0);

    /* Positive horizontal arm: row 0, x = 0..8 are white. */
    for (int x = 0; x <= 8; x++) {
        UASSERT(fb[0 * FB_W + x] == WHITE);
    }
    /* Positive vertical arm: column 0, y = 0..8 are white. */
    for (int y = 0; y <= 8; y++) {
        UASSERT(fb[y * FB_W + 0] == WHITE);
    }

    /* Past-tip pixels stay black. */
    UASSERT(fb[0 * FB_W + 9] == BLACK);
    UASSERT(fb[9 * FB_W + 0] == BLACK);

    /* Nothing should have been written to any row or column that the
     * crosshair doesn't touch — sample row 1 col 1..8 (interior of the
     * "quadrant" between the two arms). */
    UASSERT(fb[1 * FB_W + 1] == BLACK);
    UASSERT(fb[1 * FB_W + 8] == BLACK);
    UASSERT(fb[8 * FB_W + 1] == BLACK);
}

void test_draw_crosshair_suite(void) {
    utest_run("draw_crosshair_centre",        draw_crosshair_centre);
    utest_run("draw_crosshair_negative_noop", draw_crosshair_negative_noop);
    utest_run("draw_crosshair_edge_origin",   draw_crosshair_edge_origin);
}
