/* SPDX-License-Identifier: BSD-3-Clause */
/* IRQ handlers — minimal set for the Mandelbrot demo.
 *
 * SysTick_Handler delegates to HAL_IncTick for 1ms tick accounting.
 * EXTI0_IRQHandler clears the pending bit and forwards to port_button.
 * TIM2_IRQHandler clears the update flag and forwards to port_gyro_tick_isr
 *   (fires every 50 ms to inject the gyro_tick event into the urbi ring). */

#include "stm32f4xx_hal.h"

extern void port_button_exti_handler(void);
extern void port_gyro_tick_isr(void);
extern TIM_HandleTypeDef htim2;

void SysTick_Handler(void) {
    HAL_IncTick();
}

void EXTI0_IRQHandler(void) {
    if (__HAL_GPIO_EXTI_GET_IT(GPIO_PIN_0) != RESET) {
        __HAL_GPIO_EXTI_CLEAR_IT(GPIO_PIN_0);
        port_button_exti_handler();
    }
}

void TIM2_IRQHandler(void) {
    if (__HAL_TIM_GET_FLAG(&htim2, TIM_FLAG_UPDATE) != RESET &&
        __HAL_TIM_GET_IT_SOURCE(&htim2, TIM_IT_UPDATE) != RESET) {
        __HAL_TIM_CLEAR_IT(&htim2, TIM_IT_UPDATE);
        port_gyro_tick_isr();
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
