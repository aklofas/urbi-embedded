/* SPDX-License-Identifier: BSD-3-Clause */
/* examples/pico/repl_demo/main/bsp/bsp_tick.h
 *
 * 100 ms periodic timer fixture using hardware alarm 0
 * (TIMER_IRQ_0).  Each tick:
 *   1. Re-arms next alarm at now+100ms.
 *   2. Calls bsp_button_poll_isr(vm) so BOOTSEL state is sampled at
 *      100 Hz alongside the script-visible `tick` event.
 *   3. Fires the `tick` named event via urbi_inject_event.
 *
 * Two entry points:
 *   bsp_tick_register(vm)  — register the `tick` event (call from
 *                            bsp_register, before bsp_tick_start).
 *   bsp_tick_start(vm)     — arm hardware alarm 0; cannot fail today
 *                            so its return value is always 0.
 *
 * On host builds (no PICO_BOARD), both are no-op stubs. */

#ifndef URBI_PICO_BSP_TICK_H
#define URBI_PICO_BSP_TICK_H

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;

int bsp_tick_register(struct UVM *vm);
int bsp_tick_start(struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_PICO_BSP_TICK_H */
