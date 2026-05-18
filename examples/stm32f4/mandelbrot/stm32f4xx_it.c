/* SPDX-License-Identifier: BSD-3-Clause */
/* IRQ handlers — minimal set for the Mandelbrot demo.
 *
 * SysTick_Handler delegates to HAL_IncTick for 1ms tick accounting.
 * EXTI0_IRQHandler clears the pending bit and forwards to port_button. */

#include "stm32f4xx_hal.h"

extern void port_button_exti_handler(void);

void SysTick_Handler(void) {
    HAL_IncTick();
}

void EXTI0_IRQHandler(void) {
    if (__HAL_GPIO_EXTI_GET_IT(GPIO_PIN_0) != RESET) {
        __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_0);
        port_button_exti_handler();
    }
}

void HardFault_Handler(void)    { while (1); }
void MemManage_Handler(void)    { while (1); }
void BusFault_Handler(void)     { while (1); }
void UsageFault_Handler(void)   { while (1); }
void NMI_Handler(void)          { }
void SVC_Handler(void)          { }
void DebugMon_Handler(void)     { }
void PendSV_Handler(void)       { }
