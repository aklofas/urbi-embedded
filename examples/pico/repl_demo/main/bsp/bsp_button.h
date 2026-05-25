/* SPDX-License-Identifier: BSD-3-Clause */
/* examples/pico/repl_demo/main/bsp/bsp_button.h
 *
 * BOOTSEL button fixture for Pi Pico (RP2040).  Polled via the QSPI_SS
 * bit-bang trick from pico-examples/picoboard/button — there is no
 * dedicated user button on Pico, so we re-purpose BOOTSEL.
 *
 * Surface elements:
 *
 *   button_pressed() -> bool   host-fn returning the current debounced
 *                              state (true if held).
 *
 *   `pressed` named event      fired on rising-edge debounced transition
 *                              by bsp_button_poll_isr (called from the
 *                              tick ISR).
 *
 * On host builds (no PICO_BOARD), bsp_button_register is a no-op stub
 * and bsp_button_poll_isr is unused. */

#ifndef URBI_PICO_BSP_BUTTON_H
#define URBI_PICO_BSP_BUTTON_H

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;

int bsp_button_register(struct UVM *vm);

/* Called from the TIMER_IRQ_0 handler (every 100 ms).  Reads the
 * BOOTSEL line via QSPI_SS bit-bang, debounces (2 consecutive same
 * samples = state change), and on rising-edge fires the `pressed`
 * event via urbi_inject_event (ISR-safe).
 *
 * NOOP if bsp_button_register has not yet wired up the event id. */
void bsp_button_poll_isr(struct UVM *vm);

/* DEBUG: synthetic injection of the `pressed` event from the main loop.
 * Bypasses the BOOTSEL debounce path so we can verify watcher dispatch
 * works independently of the QSPI_SS bit-bang trick.  Returns 0 on
 * success, -1 if pressed_evt isn't registered yet. */
int bsp_button_inject_synthetic_pressed(struct UVM *vm);

/* Accessor for the `pressed` event id registered by bsp_button_register.
 * Used by main.c to install a C-side watcher via urbi_register_watcher,
 * bypassing the urbiscript-side `whenever (pressed)` path.  Returns
 * URBI_EVENT_ID_INVALID before bsp_button_register has been called. */
#include "urbi/urbi.h"
urbi_event_id_t bsp_button_get_pressed_evt(void);

#ifdef __cplusplus
}
#endif

#endif /* URBI_PICO_BSP_BUTTON_H */
