/* SPDX-License-Identifier: BSD-3-Clause */
/* eye_button — GPIO 0 (BOOT) ISR → urbi event injection.
 *
 * The ESP32-S3-EYE's BOOT button is wired to GPIO 0 with an internal
 * pull-up; it pulls low while pressed.  We install a NEGEDGE ISR in
 * IRAM that fires an empty-payload urbi event so the urbiscript
 * watcher `at (button_pressed) {...}` runs on the VM thread.
 *
 * Wiring contract: button_install_isr must be called once after
 * urbi_vm_init and after a urbi_event_register for the event id; the
 * (vm, ev) pair is stashed in TU-static state and read from ISR
 * context for the lifetime of the program. */

#ifndef EYE_BUTTON_H
#define EYE_BUTTON_H

#include "urbi/urbi.h"   /* struct UVM, urbi_event_id_t */

/* Install the GPIO 0 NEGEDGE ISR.  Stashes (vm, ev) in TU-static state;
 * the ISR fires urbi_inject_event(vm, ev, NULL, 0) on each press.
 * Idempotent across reboot only — second call within one boot will fail
 * the gpio_install_isr_service call (already installed). */
void button_install_isr(struct UVM *vm, urbi_event_id_t ev);

#endif /* EYE_BUTTON_H */
