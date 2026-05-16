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
 * once after urbi_vm_init.  The task takes ownership of vm and ev_blob
 * and uses them for the lifetime of the program. */
void eye_camera_init(struct UVM *vm, urbi_event_id_t ev_blob);

/* Return a previously-dequeued V4L2 buffer to the camera driver's pool.
 * Called by the display task once the blit completes — wraps
 * VIDIOC_QBUF on the camera's fd, which is camera-local state.  Returns
 * 0 on success, -1 on failure (qbuf_index out of range, ioctl failed,
 * or eye_camera_init has not run yet). */
int eye_camera_qbuf(int qbuf_index);

/* Host-fn (matches urbi_native_method_fn from <urbi/urbi.h>) that
 * urbiscript calls to retarget the blob detector to a different colour.
 *
 *   set_target_color(r, g, b, tol)
 *
 * All four args are urbiscript integers in RGB565 channel quantisation
 * (r,b: 0..31; g: 0..63; tol: per-channel window).  Writes the new target
 * under target_mux so the camera task picks it up on its next frame.
 * Returns nil. */
int c_set_target_color(struct UVM *vm, UValue self,
                       UValue *args, uint8_t nargs, UValue *out);

#endif /* EYE_CAMERA_H */
