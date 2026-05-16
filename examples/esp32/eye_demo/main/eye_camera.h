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

#include "urbi/types.h"   /* struct UVM, urbi_event_id_t */

/* Initialise the OV2640 camera with the S3-EYE v2.2 pin map, then spawn
 * the camera FreeRTOS task pinned to core 0.  Must be called once after
 * urbi_vm_init.  The task takes ownership of vm and ev_blob and uses
 * them for the lifetime of the program. */
void eye_camera_init(struct UVM *vm, urbi_event_id_t ev_blob);

#endif /* EYE_CAMERA_H */
