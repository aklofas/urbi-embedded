/* SPDX-License-Identifier: BSD-3-Clause */
/* eye_button — GPIO 0 (BOOT) ISR → urbi event injection.
 *
 * The ISR is IRAM-resident (ESP_INTR_FLAG_IRAM) so it stays valid
 * even when flash cache is suspended for SPI flash writes.
 * urbi_inject_event is the single-producer ISR-safe primitive (no
 * locks, no heap) per the v0.7.1 contract; calling it from ISR
 * context is the canonical use case.  Empty payload (NULL, 0) —
 * `at (button_pressed?)` urbi-side handlers don't destructure args.
 *
 * Debounce: mechanical switches bounce for 5-20 ms per press; the
 * BOOT button on the S3-EYE empirically fires its NEGEDGE ISR 2-4
 * times within ~100-150 ms for a single human press.  Suppress
 * follow-up ISR firings via a timestamp guard — only inject if at
 * least DEBOUNCE_US has passed since the last accepted press.
 * esp_timer_get_time() is ISR-safe and monotonic. */
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "urbi/urbi.h"

#include "eye_button.h"

#define DEBOUNCE_US (50 * 1000)   /* 50 ms */

static struct UVM       *btn_vm;
static urbi_event_id_t   btn_ev;
static volatile int64_t  last_fire_us;

static void IRAM_ATTR button_isr(void *arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (now - last_fire_us < DEBOUNCE_US) {
        return;   /* bounce within window — ignore */
    }
    last_fire_us = now;
    urbi_inject_event(btn_vm, btn_ev, NULL, 0);
}

void button_install_isr(struct UVM *vm, urbi_event_id_t ev)
{
    btn_vm = vm;
    btn_ev = ev;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    /* gpio_install_isr_service returns ESP_ERR_INVALID_STATE when
     * esp_video (camera DVP) has already installed the shared service
     * — benign; gpio_isr_handler_add below still wires our handler
     * onto the existing service. */
    (void)gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    ESP_ERROR_CHECK(gpio_isr_handler_add(GPIO_NUM_0, button_isr, NULL));
}
