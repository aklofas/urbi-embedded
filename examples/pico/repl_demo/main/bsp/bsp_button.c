/* SPDX-License-Identifier: BSD-3-Clause */
/* examples/pico/repl_demo/main/bsp/bsp_button.c
 *
 * BOOTSEL button fixture.  RP2040 has no user button on the Pico
 * board; the canonical workaround (pico-examples/picoboard/button) is
 * to bit-bang QSPI_SS as an input — the BOOTSEL switch pulls it low
 * when held.  This requires briefly disabling XIP flash, so we
 * disable interrupts around the sample.
 *
 * Sampling cadence: bsp_button_poll_isr is called from TIMER_IRQ_0
 * at 100 Hz (every 100 ms).  Debounce is "two consecutive same
 * samples confirm".  Rising edge (debounced not-held → held) fires
 * the `pressed` event via urbi_inject_event.
 *
 * urbi_inject_event is ISR-safe (single-producer ring; see
 * include/urbi/urbi.h around line 471).  button_pressed() (the
 * host-fn) is MAIN-thread only and reads the debounced flag — it is
 * not safe to call from ISR. */

#include "bsp_button.h"
#include "urbi/urbi.h"
#include "urbi/types.h"

#include <stdint.h>
#include <stdbool.h>

#ifdef PICO_BOARD
#  include "pico/stdlib.h"
#  include "hardware/structs/ioqspi.h"
#  include "hardware/structs/sio.h"
#  include "hardware/sync.h"
#endif

#ifndef PICO_BOARD
int bsp_button_register(struct UVM *vm)
{
    (void)vm;
    return 0;
}
void bsp_button_poll_isr(struct UVM *vm)
{
    (void)vm;
}
#else /* PICO_BOARD */

/* Event id registered by bsp_button_register; consulted by the
 * ISR-safe poll path.  Sentinel URBI_EVENT_ID_INVALID until set, so
 * an early ISR fire before registration completes is a no-op. */
static urbi_event_id_t s_pressed_evt = URBI_EVENT_ID_INVALID;

/* Debounce state — written by ISR, read by host-fn.  Both fields
 * fit a single machine word so torn reads are not a concern on
 * Cortex-M0+ for these single-bit signals. */
static volatile uint8_t s_last_sample  = 0U;   /* raw sample previous tick */
static volatile uint8_t s_debounced    = 0U;   /* confirmed state */

/* QSPI_SS bit-bang read of BOOTSEL.  Taken verbatim from the pico-
 * examples picoboard/button __no_inline_not_in_flash_func recipe.
 * Returns true if BOOTSEL is held.
 *
 * NOTE: the __no_inline_not_in_flash_func attribute is REQUIRED — the
 * function disables XIP while running, so its own code must already
 * be in SRAM.  Without it the chip hangs the first time it's called. */
static bool __no_inline_not_in_flash_func(read_bootsel)(void)
{
    const uint32_t CS_PIN_INDEX = 1U;

    /* Disable interrupts so the XIP outage is bounded. */
    uint32_t flags = save_and_disable_interrupts();

    /* Pull QSPI_SS high via the SW override (the flash chip drives
     * it low normally), then briefly switch its function so we can
     * read the line. */
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    /* Wait for the line to settle (~ a few cycles is plenty; the
     * pico-examples version uses a small busy loop). */
    for (volatile int i = 0; i < 1000; i++) { }

    /* BOOTSEL pulls the line LOW when held — invert. */
    bool held = (sio_hw->gpio_hi_in & (1U << CS_PIN_INDEX)) == 0U;

    /* Restore the OE override so flash works again. */
    hw_write_masked(&ioqspi_hw->io[CS_PIN_INDEX].ctrl,
                    GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
                    IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);

    restore_interrupts(flags);
    return held;
}

void bsp_button_poll_isr(struct UVM *vm)
{
    if (vm == NULL) {
        return;
    }
    uint8_t sample = read_bootsel() ? 1U : 0U;

    /* Debounce: a state change requires two consecutive matching
     * samples after the current debounced value.  Implementation:
     *   last == sample AND sample != debounced  → commit + edge. */
    bool was_held = (s_debounced != 0U);
    if (sample == s_last_sample && sample != s_debounced) {
        s_debounced = sample;
        if (!was_held && sample != 0U &&
            s_pressed_evt != URBI_EVENT_ID_INVALID) {
            /* Rising edge: fire the `pressed` event.  ISR-safe. */
            (void)urbi_inject_event(vm, (uint32_t)s_pressed_evt, NULL, 0U);
        }
    }
    s_last_sample = sample;
}

static int c_button_pressed(struct UVM *vm, UValue self,
                            UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    if (out != NULL) {
        *out = urbi_make_bool(s_debounced != 0U);
    }
    return 0;
}

int bsp_button_register(struct UVM *vm)
{
    /* No GPIO init needed — the QSPI_SS pin is owned by the flash
     * controller, we only borrow it for a few cycles in read_bootsel. */

    int rc = urbi_register(vm, NULL, "button_pressed", c_button_pressed);
    if (rc != 0) {
        return rc;
    }

    /* Register the `pressed` named event AFTER the host-fn so that an
     * early ISR fire (between bsp_register and bsp_tick_start) finds
     * s_pressed_evt still INVALID and is silently dropped — not a
     * dropped wakeup since the tick hasn't started arming yet. */
    urbi_event_id_t evt = urbi_event_register(vm, NULL, "pressed",
                                              NULL, NULL);
    if (evt == URBI_EVENT_ID_INVALID) {
        return -1;
    }
    s_pressed_evt = evt;
    return 0;
}

#endif /* PICO_BOARD */
