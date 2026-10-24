/* SPDX-License-Identifier: BSD-3-Clause */
/* eye_display — ST7789 240x240 LCD driver for the ESP32-S3-EYE demo.
 *
 * Owns SPI2 (40 MHz, 16 bpp, mode 0) and the display FreeRTOS task pinned
 * to core 1.  Receives 240x240 RGB565 frames from the camera task via
 * display_post_frame, overlays a 16-pixel crosshair on the pending blob
 * coordinates, and pushes the buffer to the panel via
 * esp_lcd_panel_draw_bitmap.
 *
 * The crosshair-overlay primitive (draw_crosshair_into) lives in the
 * sibling crosshair.h header so the host unit test
 * (tests/unit/test_draw_crosshair.c) can drive it without ESP-IDF deps —
 * same pattern as detect_blob.h for the camera blob detector. */

#ifndef EYE_DISPLAY_H
#define EYE_DISPLAY_H

#include <stdint.h>

/* Initialise SPI2 + ST7789 panel + display FreeRTOS task pinned to core 1.
 * Must be called once after esp_video_init.  The task takes ownership of
 * the frame queue for the lifetime of the program. */
void eye_display_init(void);

/* Hand a raw camera frame to the display task.  Called by the camera task
 * once per captured frame.  The buf pointer aliases a mmap'd V4L2 buffer
 * (one of the 2 returned by VIDIOC_REQBUFS on the camera's fd); qbuf_index
 * is the V4L2 buffer index, used by the display task to return the buffer
 * to the camera driver's pool via eye_camera_qbuf(qbuf_index) once the
 * blit completes.  Skipping eye_camera_qbuf starves the 2-buffer pool
 * and blocks the camera task indefinitely on its next VIDIOC_DQBUF. */
void display_post_frame(uint16_t *buf, int w, int h, int qbuf_index);

/* Set the pending crosshair coordinates in 240x240 image coordinates
 * (matching the camera blob centroid); the next frame the display task
 * processes draws it.  Writes the volatile (x, y) pair shared with the
 * display task.  Safe from any task, and before eye_display_init: the
 * pair is static.  The pair is not atomic as a pair; a torn read lasts
 * one frame and is visually invisible.  The caller clamps to 0..239. */
void display_set_crosshair(int x, int y);

#endif /* EYE_DISPLAY_H */
