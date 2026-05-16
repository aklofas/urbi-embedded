/* SPDX-License-Identifier: BSD-3-Clause */
/* eye_button — GPIO 0 (BOOT) ISR → urbi event injection.
 *
 * The ISR runs from IRAM (ESP_INTR_FLAG_IRAM) so it stays valid even
 * when flash cache is suspended for SPI flash writes.  urbi_inject_event
 * is the single-producer ISR-safe primitive (no locks, no heap) per the
 * v0.7.1 contract; calling it from ISR context is the canonical use
 * case.  Empty payload (NULL, 0) — `at (button_pressed)` does not
 * destructure arguments. */

#include "driver/gpio.h"
#include "esp_attr.h"
#include "urbi/urbi.h"

#include "eye_button.h"

static struct UVM       *btn_vm;
static urbi_event_id_t   btn_ev;

static void IRAM_ATTR button_isr(void *arg) {
    (void)arg;
    urbi_inject_event(btn_vm, btn_ev, NULL, 0);
}

void button_install_isr(struct UVM *vm, urbi_event_id_t ev) {
    btn_vm = vm; btn_ev = ev;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(GPIO_NUM_0, button_isr, NULL);
}
