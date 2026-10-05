/* SPDX-License-Identifier: BSD-3-Clause */
/* examples/pico/repl_demo/main/bsp/bsp_temp.h
 *
 * On-die temperature sensor (ADC channel 4) fixture.  Surfaces one
 * verb on Lobby:
 *
 *   temp_celsius() -> float    — read ADC4, convert to degrees Celsius */

#ifndef URBI_PICO_BSP_TEMP_H
#define URBI_PICO_BSP_TEMP_H

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;

int bsp_temp_register(struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_PICO_BSP_TEMP_H */
