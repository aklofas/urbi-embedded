/* SPDX-License-Identifier: BSD-3-Clause */
/* detect_blob_chroma — RGB565 chrominance-based single-pass centroid
 * detector.  Sibling of detect_blob.h with a different matching rule:
 *
 *   A pixel matches when its dominant channel exceeds the other two by
 *   at least `dominance` (on a 5-bit-normalized scale) AND the dominant
 *   channel itself exceeds `min_bright` (gates noise in shadows).
 *
 * Why this exists: the absolute-distance detector in detect_blob.h is
 * extremely sensitive to lighting / AWB drift.  Empirically (eye_demo
 * 2026-05-16 diagnostic data), a GREEN target with the conventional
 * RGB-distance window matched 5-13% of all pixels in typical office
 * lighting — most pixels naturally fall in the (low-r, mid-g, low-b)
 * region after the OV2640 AWB pulls everything toward green-tinted
 * shadows.  RED/BLUE were fine; GREEN was unusable for tracking.
 *
 * Chrominance fixes this: a pixel is "green" only if the green channel
 * is materially BRIGHTER than red and blue.  Robust against lighting
 * changes because it tests the relative shape, not absolute values.
 *
 * Channel normalization: green is 6-bit (0..63) while r/b are 5-bit
 * (0..31).  We right-shift green by 1 to put all three on the same
 * 5-bit scale before comparing — same approach the urbi-side BlobScan
 * uses for symmetry.
 *
 * Header-only `static inline` so the ESP-IDF target and host unit tests
 * can both consume it without duplicating the loop body.  Pure C99 —
 * depends only on <stdint.h>. */

#ifndef DETECT_BLOB_CHROMA_H
#define DETECT_BLOB_CHROMA_H

#include <stdint.h>

#include "detect_blob.h"   /* shared blob_t result type */

typedef enum {
    CHROMA_R = 0,   /* red-dominant */
    CHROMA_G = 1,   /* green-dominant */
    CHROMA_B = 2,   /* blue-dominant */
} chroma_dominant_t;

typedef struct {
    uint8_t dominant;     /* one of chroma_dominant_t (cast-safe; 0..2) */
    uint8_t dominance;    /* dom-channel must exceed both others by at least this (5-bit scale, 0..31) */
    uint8_t min_bright;   /* dom channel must be at least this bright (5-bit scale, 0..31) */
} chroma_target_t;

static inline blob_t detect_blob_chroma(const uint16_t *buf, int w, int h,
                                         chroma_target_t t)
{
    blob_t b = {0, 0, 0};
    uint64_t sx = 0, sy = 0;
    for (int i = 0, n = w * h; i < n; i++) {
        uint16_t px = buf[i];
        uint8_t r  = (px >> 11) & 0x1F;        /* 5-bit */
        uint8_t g  = ((px >>  5) & 0x3F) >> 1; /* 6-bit normalized to 5-bit */
        uint8_t bl =  px         & 0x1F;       /* 5-bit */

        uint8_t dom, other1, other2;
        switch (t.dominant) {
            case CHROMA_R: dom = r;  other1 = g;  other2 = bl; break;
            case CHROMA_G: dom = g;  other1 = r;  other2 = bl; break;
            case CHROMA_B: dom = bl; other1 = r;  other2 = g;  break;
            default: continue;
        }

        if (dom < t.min_bright) continue;
        if ((int)dom - (int)other1 < (int)t.dominance) continue;
        if ((int)dom - (int)other2 < (int)t.dominance) continue;

        b.area++;
        sx += (uint64_t)(i % w);
        sy += (uint64_t)(i / w);
    }
    if (b.area > 0) {
        b.x = (int)(sx / (uint64_t)b.area);
        b.y = (int)(sy / (uint64_t)b.area);
    }
    return b;
}

#endif /* DETECT_BLOB_CHROMA_H */
