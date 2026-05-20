/* SPDX-License-Identifier: BSD-3-Clause */
/* examples/pico/repl_demo/main/bsp/bsp_temp.h
 *
 * On-die temperature sensor (ADC channel 4) fixture.  Surfaces one
 * flat host-fn into the global realm:
 *
 *   temp_celsius() -> float    — read ADC4, convert to degrees Celsius
 *
 * (Note: a property-getter style — Lobby.temp returning the current
 * reading without parens — is not expressible via the v0.7.1
 * embedding API, so we ship a zero-arg method instead.)
 *
 * On host builds (no PICO_BOARD), bsp_temp_register is a no-op stub. */

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
