/* SPDX-License-Identifier: BSD-3-Clause */
/* USER button (PA0) wrapper for STM32F429I-DISC1.
 *
 * Configures PA0 as input with rising-edge EXTI0 interrupt.  Each press
 * injects the bound event from ISR context via urbi_inject_event, which
 * is allocation-free and delivers at the next urbi_step.
 *
 * The actual EXTI0_IRQHandler lives in the embedder (per ST naming
 * convention) and calls port_button_exti_handler() exported here. */

#include "port_stm32f4.h"
#include "urbi/urbi.h"
#include <stdint.h>

#include "stm32f4xx_hal.h"

static struct UVM *s_vm;
static urbi_event_id_t s_evt = URBI_EVENT_ID_INVALID;

void port_button_init_gpio(void) {
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
}

void port_button_bind(struct UVM *vm, urbi_event_id_t id) {
    s_vm  = vm;
    s_evt = id;
}

/* Called from EXTI0_IRQHandler on rising-edge of PA0. */
void port_button_exti_handler(void) {
    if (s_vm && s_evt != URBI_EVENT_ID_INVALID)
        (void)urbi_inject_event(s_vm, s_evt, NULL, 0);
}
