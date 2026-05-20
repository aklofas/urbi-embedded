/* SPDX-License-Identifier: BSD-3-Clause */
/* examples/pico/repl_demo/main/bsp/bsp_temp.c
 *
 * On-die temperature sensor fixture (ADC channel 4) for the Pi Pico
 * REPL demo.  RP2040 datasheet §4.9.5 — the on-die sensor is wired to
 * ADC4; voltage at 27°C is nominally 0.706 V with slope -1.721 mV/°C.
 *
 * Conversion: raw is 12-bit (0..4095); V = raw * 3.3 / 4096;
 *             T_C = 27 - (V - 0.706) / 0.001721.
 *
 * One flat host-fn:
 *   temp_celsius() -> float
 *
 * On host builds (no PICO_BOARD), bsp_temp_register is a no-op stub. */

#include "bsp_temp.h"
#include "urbi/urbi.h"
#include "urbi/types.h"

#ifdef PICO_BOARD
#  include "pico/stdlib.h"
#  include "hardware/adc.h"
#endif

#ifndef PICO_BOARD
int bsp_temp_register(struct UVM *vm)
{
    (void)vm;
    return 0;
}
#else /* PICO_BOARD */

#define PICO_TEMP_ADC_CHANNEL  4U
#define PICO_TEMP_VREF         3.3
#define PICO_TEMP_FULLSCALE    4096.0
#define PICO_TEMP_V27          0.706
#define PICO_TEMP_SLOPE        0.001721    /* V per °C (positive; subtract delta) */

static bool s_adc_inited = false;

static void temp_adc_init_once(void)
{
    if (s_adc_inited) {
        return;
    }
    adc_init();
    adc_set_temp_sensor_enabled(true);
    s_adc_inited = true;
}

static int c_temp_celsius(struct UVM *vm, UValue self,
                          UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    temp_adc_init_once();
    adc_select_input(PICO_TEMP_ADC_CHANNEL);
    uint16_t raw = adc_read();
    double v = ((double)raw * PICO_TEMP_VREF) / PICO_TEMP_FULLSCALE;
    double t_c = 27.0 - (v - PICO_TEMP_V27) / PICO_TEMP_SLOPE;
    if (out != NULL) {
        *out = urbi_make_float(t_c);
    }
    return 0;
}

int bsp_temp_register(struct UVM *vm)
{
    /* ADC init deferred to first call so that, if temp_celsius is never
     * invoked, the ADC block stays in reset (small static-current win). */
    return urbi_register(vm, NULL, "temp_celsius", c_temp_celsius);
}

#endif /* PICO_BOARD */
