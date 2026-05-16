/* SPDX-License-Identifier: BSD-3-Clause */
/* detect_blob — RGB565 single-pass tolerance-window centroid detector.
 *
 * Header-only `static inline` so the ESP-IDF target (eye_camera.c) and
 * the host unit test (tests/unit/test_detect_blob.c) can both consume the
 * same code without duplicating the loop body.  Pure C99; depends only on
 * <stdint.h> and the libc `abs` from <stdlib.h>.  Zero allocator use,
 * zero side effects — suitable for the camera FreeRTOS task hot path.
 *
 * Algorithm: one pass over `w * h` RGB565 pixels.  A pixel matches the
 * target colour `t` when each of (r, g, b) is within `t.tol` of the
 * corresponding target channel.  Accumulates (sum_x, sum_y, count) and
 * returns the centroid + count.
 *
 * Note (T25 step 1.3): the detector returns ONE centroid for ALL matching
 * pixels, not per-blob segmentation.  Multiple separate blobs collapse
 * to their joint centroid.  This is intentional — the demo expects a
 * single dominant target colour. */

#ifndef DETECT_BLOB_H
#define DETECT_BLOB_H

#include <stdint.h>
#include <stdlib.h>   /* abs */

/* RGB565 target colour in the same 5/6/5 quantisation as the framebuffer
 * pixel.  `tol` applies independently to each channel. */
typedef struct {
    uint8_t r;    /* 0..31  */
    uint8_t g;    /* 0..63  */
    uint8_t b;    /* 0..31  */
    uint8_t tol;  /* per-channel tolerance window */
} rgb565_target_t;

/* Detection result.  `area` is the count of matching pixels; (x, y) is
 * the integer-rounded centroid in image coordinates.  When area == 0 the
 * (x, y) fields are 0 and should not be consumed by the caller. */
typedef struct {
    int x;
    int y;
    int area;
} blob_t;

static inline blob_t detect_blob(const uint16_t *buf, int w, int h,
                                 rgb565_target_t t)
{
    blob_t b = {0, 0, 0};
    uint64_t sx = 0, sy = 0;
    for (int i = 0, n = w * h; i < n; i++) {
        uint16_t px = buf[i];
        uint8_t r  = (px >> 11) & 0x1F;
        uint8_t g  = (px >>  5) & 0x3F;
        uint8_t bl =  px        & 0x1F;
        if (abs((int)r  - (int)t.r) <= t.tol &&
            abs((int)g  - (int)t.g) <= t.tol &&
            abs((int)bl - (int)t.b) <= t.tol) {
            b.area++;
            sx += (uint64_t)(i % w);
            sy += (uint64_t)(i / w);
        }
    }
    if (b.area > 0) {
        b.x = (int)(sx / (uint64_t)b.area);
        b.y = (int)(sy / (uint64_t)b.area);
    }
    return b;
}

#endif /* DETECT_BLOB_H */
