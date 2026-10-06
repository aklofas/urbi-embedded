/* SPDX-License-Identifier: BSD-3-Clause */
/* Mock BSP layer for host-side unit tests of the STM32F4 port shims.
 *
 * Only the time source touches hardware the host can fake: the DWT
 * cycle counter.  Tests set mock_dwt_cyccnt and reset it with
 * mock_bsp_reset() before exercising the shim under test.
 *
 * This header MUST be included BEFORE the port shim TU under test,
 * so the macro below shadows the register read at compile time. */
#ifndef URBI_TEST_MOCK_BSP_H
#define URBI_TEST_MOCK_BSP_H

#include <stdint.h>

/* ---- DWT cycle-counter mock ---- */

extern uint32_t mock_dwt_cyccnt;

/* port_time.c reads DWT->CYCCNT directly; the mock substitutes the
 * variable via the macro below. */
#define DWT_CYCCNT_READ()  (mock_dwt_cyccnt)

/* ---- Reset all mock state to zero (call from each test setup) ---- */

void mock_bsp_reset(void);

#endif /* URBI_TEST_MOCK_BSP_H */
