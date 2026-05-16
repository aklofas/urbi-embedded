/* SPDX-License-Identifier: BSD-3-Clause */
/* eye_camera — OV2640 capture + blob detection driver for the
 * ESP32-S3-EYE demo.  Owns the camera FreeRTOS task; pushes detected
 * blob centroids into the urbi VM via urbi_inject_event and forwards
 * raw frames to the display task.
 *
 * The blob detector itself (detect_blob) is host-compilable and lives
 * in detect_blob.h so unit tests can drive it without ESP-IDF headers.
 */

#ifndef EYE_CAMERA_H
#define EYE_CAMERA_H

#include "urbi/urbi.h"   /* struct UVM, urbi_event_id_t, UValue, urbi_native_method_fn */

/* Initialise the OV2640 camera via esp_video with the S3-EYE v2.2 pin
 * map, open /dev/video2, request + mmap 2 buffers, start streaming,
 * then spawn the camera FreeRTOS task pinned to core 0.  Must be called
 * once after urbi_vm_init.  The task takes ownership of vm, ev_blob,
 * and ev_scan_tick and uses them for the lifetime of the program.
 *
 * Also allocates the urbiscript-side snap buffer (115 KB in PSRAM): every
 * SCAN_TICK_EVERY_N_FRAMES captured frame the camera task memcpy's its
 * RGB565 pixels into this buffer and injects ev_scan_tick.  The urbi-side
 * BlobScan class then reads pixels via c_get_pixel_{r,g,b} into the same
 * memory while the camera task continues its own DQBUF/QBUF cycle on the
 * V4L2 buffers (no contention — snap buffer is a private copy). */
void eye_camera_init(struct UVM *vm,
                     urbi_event_id_t ev_blob,
                     urbi_event_id_t ev_scan_tick);

/* Return a previously-dequeued V4L2 buffer to the camera driver's pool.
 * Called by the display task once the blit completes — wraps
 * VIDIOC_QBUF on the camera's fd, which is camera-local state.  Returns
 * 0 on success, -1 on failure (qbuf_index out of range, ioctl failed,
 * or eye_camera_init has not run yet). */
int eye_camera_qbuf(int qbuf_index);

/* Host fn — push a chrominance target from urbiscript to the camera task.
 *
 *   set_target_chroma(dominant, dominance, min_bright)
 *
 * Replaces the legacy `set_target_color` (RGB-distance) — see
 * detect_blob_chroma.h for rationale.  Returns nil. */
int c_set_target_chroma(struct UVM *vm, UValue self,
                        UValue *args, uint8_t nargs, UValue *out);

/* Host fns: read individual RGB channels from the urbiscript snap buffer.
 *
 *   c_get_pixel_r(x, y)  // returns int in 0..31 (RGB565 5-bit red)
 *   c_get_pixel_g(x, y)  // returns int in 0..63 (RGB565 6-bit green)
 *   c_get_pixel_b(x, y)  // returns int in 0..31 (RGB565 5-bit blue)
 *
 * The (x, y) integer args are clamped to the frame bounds (240x240); out-of-
 * range reads return 0 silently.  Quantisation matches the
 * `rgb565_target_t` channels in detect_blob.h — urbiscript can compare
 * channel reads directly against the Color.new().init(r,g,b,...) tolerance
 * window values without any 0-255 rescaling. */
int c_get_pixel_r(struct UVM *vm, UValue self,
                  UValue *args, uint8_t nargs, UValue *out);
int c_get_pixel_g(struct UVM *vm, UValue self,
                  UValue *args, uint8_t nargs, UValue *out);
int c_get_pixel_b(struct UVM *vm, UValue self,
                  UValue *args, uint8_t nargs, UValue *out);

/* Scan-in-progress guard (option C).  urbiscript calls c_scan_begin at
 * the top of BlobScan.scan() and c_scan_end at the bottom; the camera
 * task uses this to throttle scan_tick injection so a slow urbiscript
 * scan can't overflow the SPSC event ring. */
int c_scan_begin(struct UVM *vm, UValue self,
                 UValue *args, uint8_t nargs, UValue *out);
int c_scan_end(struct UVM *vm, UValue self,
               UValue *args, uint8_t nargs, UValue *out);

/* Diagnostic telemetry (2026-05-16).  Per-color blob probe count,
 * frame-center RGB sample, DQBUF wait histogram.  See eye_camera.c
 * for cadence (PROBE_EVERY_N_FRAMES = 0.5 Hz). */
int c_probe_red          (struct UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
int c_probe_green        (struct UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
int c_probe_blue         (struct UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
int c_center_r           (struct UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
int c_center_g           (struct UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
int c_center_b           (struct UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
int c_dqbuf_max_wait_us  (struct UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);

#endif /* EYE_CAMERA_H */
