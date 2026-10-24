/* SPDX-License-Identifier: BSD-3-Clause */
/* A FreeRTOS queue and the inject hook, mocked for the host: the queue is
 * a mutex-guarded ring driven from pthreads; the inject hook records
 * which thread called it and with what. */
#ifndef URBI_TEST_MOCK_FREERTOS_H
#define URBI_TEST_MOCK_FREERTOS_H
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include "urbi/types.h"

typedef void *QueueHandle_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks);
BaseType_t xQueueSendFromISR(QueueHandle_t q, const void *item, BaseType_t *woken);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks);
BaseType_t xQueuePeek(QueueHandle_t q, void *item, TickType_t ticks);

/* What the mock saw urbi_inject_event do. */
typedef struct { urbi_event_id_t id; uint64_t payload; size_t n; pthread_t thread; } mock_inject_rec;
extern mock_inject_rec mock_injects[4096];
extern size_t mock_inject_count;
void mock_freertos_reset(void);
#endif
