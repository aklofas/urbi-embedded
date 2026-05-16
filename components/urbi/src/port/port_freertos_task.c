/* SPDX-License-Identifier: BSD-3-Clause */
/* ESP-IDF FreeRTOS task glue — drives urbi_step in a budget-bounded loop
 * and provides the ISR-safe wake hook for urbi_set_wake_fn.
 *
 * Two pieces:
 *
 *   port_urbi_task_body
 *     FreeRTOS task entry.  Pass to xTaskCreate / xTaskCreateStatic with
 *     `arg` set to a `struct UVM *`.  Calls urbi_step in a loop and parks
 *     the task on a task-notification when the VM is QUIESCENT or
 *     WAKE_AT (with a millisecond timeout in the latter case).  On
 *     URBI_STEP_FATAL the task currently calls esp_restart(); embedders
 *     who want a different policy fork this file.
 *
 *   port_wake_from_inject
 *     urbi_wake_fn callback (include/urbi/urbi.h:612 — `void (*)(void *ud)`).
 *     `ud` must point to a TaskHandle_t for the urbi task.  Safe to call
 *     from ISR or task context — dispatches via xPortInIsrContext.  Pass
 *     to urbi_set_wake_fn(vm, port_wake_from_inject, &urbi_task_handle).
 *
 * Compile-time tunables (override via -D... at build time):
 *   URBI_STACK_WORDS   default 8 KB / sizeof(StackType_t).  Used by
 *                      embedders that declare static stack buffers in
 *                      app_main; this file does not allocate any stack.
 *   URBI_STEP_BUDGET   default 256.  Instructions per urbi_step call;
 *                      smaller = lower scheduling latency, more overhead.
 *
 * urbi_step typedef:    include/urbi/urbi.h:177
 * urbi_wake_fn typedef: include/urbi/urbi.h:612 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

#include "urbi/urbi.h"
#include "port_esp_idf.h"

#ifndef URBI_STACK_WORDS
#  define URBI_STACK_WORDS  (8 * 1024 / sizeof(StackType_t))
#endif
#ifndef URBI_STEP_BUDGET
#  define URBI_STEP_BUDGET  256
#endif

void port_wake_from_inject(void *ud)
{
    BaseType_t hpw = pdFALSE;
    if (xPortInIsrContext()) {
        vTaskNotifyGiveFromISR(*(TaskHandle_t *)ud, &hpw);
        portYIELD_FROM_ISR(hpw);
    } else {
        xTaskNotifyGive(*(TaskHandle_t *)ud);
    }
}

/* Optional step-instrumentation hook.  An app TU that wants to count
 * urbi_step result-distribution can define these as strong symbols; the
 * weak defaults here are no-ops so embedders pay no cost by default.
 *
 * Used by examples/esp32/eye_demo to expose c_step_running / quiescent /
 * wake_at / fatal counters to the urbiscript-side Stats class. */
__attribute__((weak)) void port_urbi_step_observed(int result_int, uint64_t wake_us)
{
    (void)result_int;
    (void)wake_us;
}

void port_urbi_task_body(void *arg)
{
    struct UVM *vm = (struct UVM *)arg;
    for (;;) {
        uint64_t wake_us = 0;
        UStepResult r = urbi_step(vm, URBI_STEP_BUDGET, &wake_us);
        port_urbi_step_observed((int)r, wake_us);
        switch (r) {
            case URBI_STEP_RUNNING:
                break;
            case URBI_STEP_QUIESCENT:
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                break;
            case URBI_STEP_WAKE_AT:
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wake_us / 1000 + 1));
                break;
            case URBI_STEP_FATAL:
                /* Embedder-overridable: default is restart. */
                esp_restart();
                break;
        }
    }
}
