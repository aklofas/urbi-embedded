/* SPDX-License-Identifier: BSD-3-Clause */
/* examples/pico/repl_demo/main/bsp/bsp_tick.c
 *
 * 100 ms periodic tick using RP2040 hardware alarm 0 (TIMER_IRQ_0).
 *
 * The handler:
 *   1. Re-arms hardware alarm 0 for now + 100 ms (one-shot timer
 *      requires re-arm in the ISR).
 *   2. Polls the BOOTSEL button — bsp_button_poll_isr handles
 *      debounce + rising-edge `pressed` event injection.
 *   3. Injects the `tick` named event.
 *
 * Period is fixed at 100 ms (10 Hz) — balances responsiveness of
 * the button-debounce loop against ISR overhead.  Embedders who want
 * a different cadence can edit PICO_TICK_PERIOD_US below. */

#include "bsp_tick.h"
#include "bsp_button.h"
#include "urbi/urbi.h"

#include <stdint.h>

#ifdef PICO_BOARD
#  include "pico/stdlib.h"
#  include "hardware/timer.h"
#  include "hardware/irq.h"
#endif

#ifndef PICO_BOARD
int bsp_tick_register(struct UVM *vm)
{
    (void)vm;
    return 0;
}
int bsp_tick_start(struct UVM *vm)
{
    (void)vm;
    return 0;
}
#else /* PICO_BOARD */

#define PICO_TICK_PERIOD_US   100000U   /* 100 ms = 10 Hz */
#define PICO_TICK_ALARM_NUM   0U        /* hardware alarm 0 → TIMER_IRQ_0 */

/* File-statics — read by ISR, written by bsp_tick_{register,start}. */
static struct UVM       *s_tick_vm   = NULL;
static urbi_event_id_t   s_tick_evt  = URBI_EVENT_ID_INVALID;

static void tick_alarm_isr(void)
{
    /* Clear the IRQ latch first; otherwise re-arming the next alarm
     * may race with a missed-clear and we lose ticks. */
    hw_clear_bits(&timer_hw->intr, 1U << PICO_TICK_ALARM_NUM);

    /* Re-arm for the next tick.  hardware_alarm_set_target uses an
     * absolute time; using current + period is correct (one-shot
     * timer). */
    uint64_t now = time_us_64();
    timer_hw->alarm[PICO_TICK_ALARM_NUM] =
        (uint32_t)(now + (uint64_t)PICO_TICK_PERIOD_US);

    if (s_tick_vm == NULL) {
        return;
    }

    /* Sample BOOTSEL.  Internally calls urbi_inject_event on rising
     * edge; ISR-safe. */
    bsp_button_poll_isr(s_tick_vm);

    /* Fire the `tick` event.  ISR-safe single-producer ring. */
    if (s_tick_evt != URBI_EVENT_ID_INVALID) {
        (void)urbi_inject_event(s_tick_vm, (uint32_t)s_tick_evt,
                                NULL, 0U);
    }
}

int bsp_tick_register(struct UVM *vm)
{
    urbi_event_id_t evt = urbi_event_register(vm, NULL, "tick",
                                              NULL, NULL);
    if (evt == URBI_EVENT_ID_INVALID) {
        return -1;
    }
    s_tick_evt = evt;
    return 0;
}

int bsp_tick_start(struct UVM *vm)
{
    if (vm == NULL) {
        return -1;
    }
    s_tick_vm = vm;

    /* Install ISR + enable IRQ line BEFORE arming the alarm — if the
     * first alarm fires before the handler is installed, the default
     * weak handler hangs. */
    irq_set_exclusive_handler(TIMER_IRQ_0, tick_alarm_isr);
    irq_set_enabled(TIMER_IRQ_0, true);
    hw_set_bits(&timer_hw->inte, 1U << PICO_TICK_ALARM_NUM);

    /* Arm first alarm. */
    uint64_t now = time_us_64();
    timer_hw->alarm[PICO_TICK_ALARM_NUM] =
        (uint32_t)(now + (uint64_t)PICO_TICK_PERIOD_US);
    return 0;
}

#endif /* PICO_BOARD */
