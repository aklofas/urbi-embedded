/* SPDX-License-Identifier: BSD-3-Clause */
/* eye_display — ST7789 240x240 LCD driver for the ESP32-S3-EYE demo.
 *
 * Owns SPI2 (40 MHz, 16 bpp, mode 0) and the display FreeRTOS task pinned
 * to core 1.  Receives raw QVGA (320x240) RGB565 frames from the camera
 * task via display_post_frame, centre-crops them to 240x240, overlays a
 * 16-pixel crosshair on the pending blob coordinates, and pushes the
 * resulting buffer to the panel via esp_lcd_panel_draw_bitmap.
 *
 * The crosshair-overlay primitive (draw_crosshair_into) lives in the
 * sibling crosshair.h header so the host unit test
 * (tests/unit/test_draw_crosshair.c) can drive it without ESP-IDF deps —
 * same pattern as detect_blob.h for the camera blob detector. */

#ifndef EYE_DISPLAY_H
#define EYE_DISPLAY_H

#include "esp_camera.h"  /* camera_fb_t — frame handoff from eye_camera.c */

#include "urbi/urbi.h"   /* struct UVM, UValue, urbi_native_method_fn */

/* Initialise SPI2 + ST7789 panel + display FreeRTOS task pinned to core 1.
 * Must be called once after esp_camera_init.  The task takes ownership of
 * the frame queue for the lifetime of the program. */
void eye_display_init(void);

/* Hand a raw camera frame to the display task.  Called by the camera task
 * once per captured frame.  Per the ownership contract documented in
 * eye_camera.c lines 49-53, the display task TAKES OWNERSHIP of fb and
 * MUST call esp_camera_fb_return(fb) once the blit completes, otherwise
 * the camera driver's 2-buffer pool starves. */
void display_post_frame(camera_fb_t *fb);

/* Host-fn (matches urbi_native_method_fn from <urbi/urbi.h>) that
 * urbiscript calls to update the pending crosshair coordinates:
 *
 *   draw_crosshair(x, y)
 *
 * Both args are urbiscript integers in 320x240 image coordinates (matching
 * the camera blob centroid).  The crosshair appears on the next frame the
 * display task processes.  Returns nil.
 *
 * Signature note: the brainstorm spec sketch in §5.3 used a `UStrand *` /
 * `UValue` return convention that does not match the real v0.7.1 surface —
 * corrected here to the canonical urbi_native_method_fn shape (same drift
 * as T28 c_set_target_color; see eye_camera.c:104-115). */
int c_draw_crosshair(struct UVM *vm, UValue self,
                     UValue *args, uint8_t nargs, UValue *out);

#endif /* EYE_DISPLAY_H */
