/* SPDX-License-Identifier: BSD-3-Clause */
/* The event queue: the one way the camera task and the button interrupt
 * reach the VM.  Producers post fixed-size records into a FreeRTOS queue
 * without blocking; a full queue refuses the record and counts the drop.
 * The task that owns the VM drains the queue and is the only caller of
 * urbi_inject_event, so the runtime's single-producer ring sees exactly
 * one producer.
 *
 * port_queue_post_from_isr does not yield.  An interrupt that wants the
 * VM task to run sooner calls portYIELD_FROM_ISR itself after posting. */
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "urbi/urbi.h"
#include "port_esp_idf.h"

static QueueHandle_t s_q;
static uint32_t s_dropped;
static uint32_t s_inject_failed;

int port_queue_init(size_t depth)
{
    s_q = xQueueCreate((UBaseType_t)depth, sizeof(port_event_rec));
    return s_q != NULL ? 0 : -1;
}

int port_queue_post(const port_event_rec *rec)
{
    if (xQueueSend(s_q, rec, 0) == pdTRUE) return 0;
    s_dropped++;
    return -1;
}

int port_queue_post_from_isr(const port_event_rec *rec)
{
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_q, rec, &woken) == pdTRUE) return 0;
    s_dropped++;
    return -1;
}

size_t port_queue_drain(struct UVM *vm)
{
    size_t n = 0;
    port_event_rec rec;
    while (xQueueReceive(s_q, &rec, 0) == pdTRUE) {
        urbi_event_payload_t p;
        p.u64[0] = rec.payload;
        /* With n == 0 the runtime copies nothing, so the buffer is passed
         * either way; only the length says whether there is a payload. */
        if (urbi_inject_event(vm, rec.id, &p, rec.has_payload ? 8U : 0U) != URBI_OK) {
            s_inject_failed++;
        }
        n++;
    }
    return n;
}

int port_queue_wait(uint32_t timeout_ms)
{
    port_event_rec rec;
    return xQueuePeek(s_q, &rec, pdMS_TO_TICKS(timeout_ms)) == pdTRUE ? 1 : 0;
}

uint32_t port_queue_dropped(void)
{
    return s_dropped;
}

uint32_t port_queue_inject_failures(void)
{
    return s_inject_failed;
}
