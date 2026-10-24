/* SPDX-License-Identifier: BSD-3-Clause */
/* eye_button — GPIO 0 (BOOT) interrupt -> the port's event queue.
 *
 * The handler is IRAM_ATTR and the camera's DVP driver installs the GPIO
 * service with ESP_INTR_FLAG_IRAM, but port_queue_post_from_isr is
 * linked in flash: a press while the flash cache is off (a flash write
 * or erase) would fault.  The demo writes no flash at run time.  The
 * handler posts an empty-payload record, which never blocks; the VM task
 * is the only caller of urbi_inject_event.
 *
 * Debounce: the BOOT button bounces 2-4 times within ~150 ms per
 * physical press.  Filtering lives in the workload's button handler
 * (see eye_demo.u), not here — it demonstrates a reactive pattern using
 * System.time_us().  The cost is a few extra records per press, well
 * within the queue's depth.  An embedder that wants interrupt-level
 * suppression can add an esp_timer_get_time() guard here. */
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "port_esp_idf.h"

#include "eye_button.h"

static urbi_event_id_t   btn_ev;

static void IRAM_ATTR button_isr(void *arg)
{
    (void)arg;
    port_event_rec r = { .id = btn_ev, .has_payload = 0 };
    (void)port_queue_post_from_isr(&r);
}

void eye_button_bind(urbi_event_id_t ev)
{
    btn_ev = ev;
}

void eye_button_init(void)
{
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
