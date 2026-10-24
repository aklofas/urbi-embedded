/* SPDX-License-Identifier: BSD-3-Clause */
#include "mock_freertos.h"
#include <stdlib.h>
#include <string.h>
#include "urbi/urbi.h"

typedef struct { pthread_mutex_t mu; pthread_cond_t cv; unsigned char *items; size_t len, item, head, count; } mq;

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size)
{
    mq *q = (mq *)calloc(1, sizeof *q);
    q->items = (unsigned char *)calloc(len, item_size);
    q->len = len; q->item = item_size;
    pthread_mutex_init(&q->mu, NULL); pthread_cond_init(&q->cv, NULL);
    return q;
}
BaseType_t xQueueSend(QueueHandle_t h, const void *item, TickType_t ticks)
{
    (void)ticks;                      /* the port always posts with 0 ticks */
    mq *q = (mq *)h;
    pthread_mutex_lock(&q->mu);
    if (q->count == q->len) { pthread_mutex_unlock(&q->mu); return pdFALSE; }
    memcpy(q->items + ((q->head + q->count) % q->len) * q->item, item, q->item);
    q->count++;
    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return pdTRUE;
}
BaseType_t xQueueSendFromISR(QueueHandle_t h, const void *item, BaseType_t *woken)
{
    if (woken) *woken = pdFALSE;
    return xQueueSend(h, item, 0);
}
BaseType_t xQueueReceive(QueueHandle_t h, void *item, TickType_t ticks)
{
    mq *q = (mq *)h;
    pthread_mutex_lock(&q->mu);
    if (q->count == 0 && ticks != 0) {
        if (ticks == portMAX_DELAY) pthread_cond_wait(&q->cv, &q->mu);
        else { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
               ts.tv_nsec += (long)(ticks % 1000) * 1000000L; ts.tv_sec += ticks / 1000 + ts.tv_nsec / 1000000000L;
               ts.tv_nsec %= 1000000000L; pthread_cond_timedwait(&q->cv, &q->mu, &ts); }
    }
    if (q->count == 0) { pthread_mutex_unlock(&q->mu); return pdFALSE; }
    memcpy(item, q->items + q->head * q->item, q->item);
    q->head = (q->head + 1) % q->len; q->count--;
    pthread_mutex_unlock(&q->mu);
    return pdTRUE;
}

/* xQueueReceive without the removal: blocks until an item exists. */
BaseType_t xQueuePeek(QueueHandle_t h, void *item, TickType_t ticks)
{
    mq *q = (mq *)h;
    pthread_mutex_lock(&q->mu);
    if (q->count == 0 && ticks != 0) {
        if (ticks == portMAX_DELAY) pthread_cond_wait(&q->cv, &q->mu);
        else { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
               ts.tv_nsec += (long)(ticks % 1000) * 1000000L; ts.tv_sec += ticks / 1000 + ts.tv_nsec / 1000000000L;
               ts.tv_nsec %= 1000000000L; pthread_cond_timedwait(&q->cv, &q->mu, &ts); }
    }
    if (q->count == 0) { pthread_mutex_unlock(&q->mu); return pdFALSE; }
    memcpy(item, q->items + q->head * q->item, q->item);
    pthread_mutex_unlock(&q->mu);
    return pdTRUE;
}

mock_inject_rec mock_injects[4096];
size_t mock_inject_count;
/* The runtime's entry point, replaced: the test links no liburbi. */
int urbi_inject_event(UVM *vm, urbi_event_id_t id, const urbi_event_payload_t *payload, size_t n)
{
    (void)vm;
    if (mock_inject_count < 4096) {
        mock_injects[mock_inject_count].id = id;
        mock_injects[mock_inject_count].payload = payload ? payload->u64[0] : 0;
        mock_injects[mock_inject_count].n = n;
        mock_injects[mock_inject_count].thread = pthread_self();
    }
    mock_inject_count++;
    return URBI_OK;
}
void mock_freertos_reset(void) { mock_inject_count = 0; memset(mock_injects, 0, sizeof mock_injects); }
