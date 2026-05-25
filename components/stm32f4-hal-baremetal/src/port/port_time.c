/* SPDX-License-Identifier: BSD-3-Clause */
/* Monotonic-microseconds time source via ARM Cortex-M DWT cycle counter.
 *
 * The DWT (Data Watchpoint and Trace) unit's CYCCNT register increments
 * once per CPU cycle.  At 180 MHz, 1 us == 180 cycles.  The counter is
 * a uint32 that wraps every ~24 seconds at 180 MHz; we maintain a
 * 64-bit accumulator to give wrap-free monotonicity. */

#include "port_stm32f4.h"
#include <stdint.h>

#ifdef URBI_PORT_TEST
/* Host-side test build: mock_bsp.h provides DWT_CYCCNT_READ macro */
#  include "mock_bsp.h"
#else
#  include "stm32f4xx.h"
#  define DWT_CYCCNT_READ()  (DWT->CYCCNT)
#endif

#ifndef URBI_CPU_HZ
#  define URBI_CPU_HZ  180000000U  /* STM32F429 default after SystemClock_Config */
#endif

#define CYCLES_PER_US  (URBI_CPU_HZ / 1000000U)

static uint32_t last_cyc;
static uint64_t accum_cyc;

uint64_t port_time_us(void *ud) {
    (void)ud;
    uint32_t now = DWT_CYCCNT_READ();
    uint32_t delta = now - last_cyc;  /* unsigned wrap-around handles overflow */
    accum_cyc += delta;
    last_cyc = now;
    return accum_cyc / CYCLES_PER_US;
}
