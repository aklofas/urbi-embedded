/* SPDX-License-Identifier: BSD-3-Clause */
/* eye_camera — OV2640 capture + blob detection driver for the
 * ESP32-S3-EYE demo.  Owns the camera FreeRTOS task; posts each detected
 * blob to the port's event queue and forwards raw frames to the display
 * task.  The task never touches the VM: the VM task drains the queue.
 *
 * The blob detectors are host-compilable and live in detect_blob.h and
 * detect_blob_chroma.h so unit tests can drive them without ESP-IDF
 * headers.
 */

#ifndef EYE_CAMERA_H
#define EYE_CAMERA_H

#include "urbi/types.h"            /* urbi_event_id_t */
#include "detect_blob_chroma.h"    /* chroma_target_t */

/* The event id each blob record carries.  Call before eye_camera_init. */
void eye_camera_bind(urbi_event_id_t blob_ev);

/* Initialise the OV2640 camera via esp_video with the S3-EYE v2.2 pin
 * map, open the DVP V4L2 device, request + mmap 2 buffers, start
 * streaming, then spawn the camera FreeRTOS task pinned to core 0.  Each
 * frame with a blob posts one record: x in bits 0-15, y in bits 16-31,
 * the pixel count from bit 32. */
void eye_camera_init(void);

/* Set the chrominance target the camera task matches against.  Safe from
 * any task and before eye_camera_init: it writes a static under a
 * critical section. */
void eye_camera_set_target(chroma_target_t t);

/* Return a previously-dequeued V4L2 buffer to the camera driver's pool.
 * Called by the display task once the blit completes — wraps
 * VIDIOC_QBUF on the camera's fd, which is camera-local state.  Returns
 * 0 on success, -1 on failure (qbuf_index out of range, ioctl failed,
 * or eye_camera_init has not run yet). */
int eye_camera_qbuf(int qbuf_index);

#endif /* EYE_CAMERA_H */
