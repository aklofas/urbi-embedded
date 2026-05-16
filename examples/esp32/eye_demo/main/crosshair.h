/* SPDX-License-Identifier: BSD-3-Clause */
/* crosshair — 16-pixel cross overlay primitive for the ESP32-S3-EYE
 * eye_demo display path.
 *
 * Header-only `static inline` so the ESP-IDF target (eye_display.c) and
 * the host unit test (tests/unit/test_draw_crosshair.c) can both consume
 * the same code without duplicating the loop body — same split as
 * detect_blob.h for the camera-side blob detector.  Pure C99; depends
 * only on <stdint.h>.
 *
 * Draws a 16-pixel (actually 17 — dx=-8..+8 inclusive, dy likewise) white
 * (0xFFFF) cross centred on (cx, cy) directly into an RGB565 framebuffer
 * of dimensions w × h.  Negative coordinates are treated as a sentinel
 * meaning "no crosshair pending" and are a clean no-op.  Out-of-bounds
 * arms of the cross are clipped per-pixel; the centred-on-edge case
 * draws only the on-buffer half. */

#ifndef CROSSHAIR_H
#define CROSSHAIR_H

#include <stdint.h>

static inline void draw_crosshair_into(uint16_t *fb, int w, int h,
                                       int cx, int cy)
{
    if (cx < 0 || cy < 0) return;
    uint16_t color = 0xFFFF;  /* white */
    for (int dx = -8; dx <= 8; dx++)
        if (cx + dx >= 0 && cx + dx < w) fb[cy * w + cx + dx] = color;
    for (int dy = -8; dy <= 8; dy++)
        if (cy + dy >= 0 && cy + dy < h) fb[(cy + dy) * w + cx] = color;
}

#endif /* CROSSHAIR_H */
