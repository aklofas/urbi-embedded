/* SPDX-License-Identifier: BSD-3-Clause */
/* The board's script surface.
 *
 * Verbs are installed on the Lobby prototype, which sits in every
 * realm's chain, so `led_on()` resolves unqualified from the boot
 * workload and from each REPL session's own realm, the same way `echo`
 * does.  Events are stored on Object for the same reach: Lobby refuses
 * slot writes, Object does not.
 *
 *   led_on()  led_off()  led_toggle()  led_pwm(duty 0.0..1.0)
 *   temp_celsius() -> Float          button_pressed() -> Boolean
 *   pressed (event, BOOTSEL rising edge)   tick (event, every 100 ms)
 *
 * Order: bsp_register after urbi_open; bsp_tick_start after the
 * workload is loaded and the eval service is up, so the first ticks
 * have something to reach. */
#ifndef REPL_DEMO_BSP_REGISTER_H
#define REPL_DEMO_BSP_REGISTER_H

#include "urbi/urbi.h"

int bsp_register(struct UVM *vm);
int bsp_tick_start(struct UVM *vm);

/* Registers `name` for interrupt injection and stores the event where
 * script will find it (Object.<name>).  The id is what an interrupt
 * handler passes to urbi_inject_event. */
int bsp_bind_event(struct UVM *vm, const char *name, urbi_event_id_t *out_id);

#endif
