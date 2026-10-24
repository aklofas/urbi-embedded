/* SPDX-License-Identifier: BSD-3-Clause */
/* eye_button — GPIO 0 (BOOT) interrupt -> one record on the port's event
 * queue per falling edge.
 *
 * The ESP32-S3-EYE's BOOT button is wired to GPIO 0 with an internal
 * pull-up; it pulls low while pressed.  The interrupt posts an
 * empty-payload record; the VM task drains the queue and the workload's
 * `at (button_pressed?)` runs there. */

#ifndef EYE_BUTTON_H
#define EYE_BUTTON_H

#include "urbi/types.h"   /* urbi_event_id_t */

/* The event id each press record carries.  Call before eye_button_init. */
void eye_button_bind(urbi_event_id_t ev);

/* Configure GPIO 0 and install the NEGEDGE handler.  Once per boot: a
 * second call fails gpio_isr_handler_add. */
void eye_button_init(void);

#endif /* EYE_BUTTON_H */
