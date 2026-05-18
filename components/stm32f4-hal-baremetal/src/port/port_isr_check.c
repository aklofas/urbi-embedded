/* SPDX-License-Identifier: BSD-3-Clause */
/* ISR-context predicate using ARM Cortex-M IPSR register.
 *
 * IPSR (Interrupt Program Status Register) is 0 in thread mode and equal
 * to the active IRQ number in handler (ISR) mode.  __get_IPSR() is a
 * core_cmFunc.h intrinsic from CMSIS. */

#include "port_stm32f4.h"
#include "stm32f4xx.h"  /* CMSIS device header — pulls in __get_IPSR */

bool port_in_isr(void) {
    return __get_IPSR() != 0;
}
