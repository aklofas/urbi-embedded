/* SPDX-License-Identifier: BSD-3-Clause */
/* USER button (PA0) wrapper for STM32F429I-DISC1.
 *
 * Configures PA0 as input with rising-edge EXTI0 interrupt.  Each press
 * emits the registered event into the urbi event ring from ISR context
 * via urbi_event_emit_from_isr.
 *
 * The actual EXTI0_IRQHandler lives in stm32f4xx_it.c (per ST naming
 * convention) and calls port_button_exti_handler() exported here. */

#include "port_stm32f4.h"
#include <stdint.h>

#ifdef URBI_PORT_TEST
#  include "mock_bsp.h"
#else
#  include "stm32f4xx_hal.h"
   extern int urbi_event_emit_from_isr(void *vm, uint32_t event_id, void *payload);
#endif

static struct UVM *bound_vm;
static uint32_t bound_event_id;

void port_button_init_gpio(void) {
#ifndef URBI_PORT_TEST
    __HAL_RCC_GPIOA_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {
        .Pin   = GPIO_PIN_0,
        .Mode  = GPIO_MODE_IT_RISING,
        .Pull  = GPIO_PULLDOWN,
        .Speed = GPIO_SPEED_FREQ_LOW,
    };
    HAL_GPIO_Init(GPIOA, &gpio);
    /* Priority: above SysTick default (15) so SysTick stays preemptible by
     * EXTI0 — standard embedded convention. */
    HAL_NVIC_SetPriority(EXTI0_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(EXTI0_IRQn);
#endif
}

void port_button_init(struct UVM *vm, uint32_t event_id) {
    bound_vm       = vm;
    bound_event_id = event_id;
}

/* Called from EXTI0_IRQHandler (stm32f4xx_it.c) on rising-edge of PA0. */
void port_button_exti_handler(void) {
    if (bound_vm != 0) {
        urbi_event_emit_from_isr(bound_vm, bound_event_id, 0);
    }
}
