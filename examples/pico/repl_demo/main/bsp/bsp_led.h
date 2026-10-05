/* SPDX-License-Identifier: BSD-3-Clause */
/* examples/pico/repl_demo/main/bsp/bsp_led.h
 *
 * GPIO25 on-board LED fixture.  Surfaces four verbs on Lobby:
 *
 *   led_on()              — drive GPIO25 high
 *   led_off()             — drive GPIO25 low
 *   led_toggle()          — XOR GPIO25
 *   led_pwm(duty)         — 0.0..1.0 duty cycle via PWM (slice 4, chan B)
 *
 * Hardware: GP25, PWM slice 4 channel B. */

#ifndef URBI_PICO_BSP_LED_H
#define URBI_PICO_BSP_LED_H

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;

int bsp_led_register(struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_PICO_BSP_LED_H */
