/* SPDX-License-Identifier: BSD-3-Clause */
/* The panel is 240x240: draw_crosshair clamps each coordinate into it, here so the host test pins the same code. */
#ifndef EYE_CLAMP_H
#define EYE_CLAMP_H

static inline int eye_clamp240(int v) { return v < 0 ? 0 : v > 239 ? 239 : v; }

#endif /* EYE_CLAMP_H */
