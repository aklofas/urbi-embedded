/* SPDX-License-Identifier: BSD-3-Clause */
#include "mock_freertos.h"
#include "../../components/esp32-idf/src/port/port_esp_idf.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Two producers post concurrently; only the draining thread injects, and
 * every record it drains reaches the runtime exactly once, in each
 * producer's order. */
#define PER_PRODUCER 500
static void *producer(void *arg)
{
    uintptr_t base = (uintptr_t)arg;           /* 1 = camera (payload), 2 = button (no payload) */
    for (unsigned i = 0; i < PER_PRODUCER; i++) {
        port_event_rec r; memset(&r, 0, sizeof r);
        r.id = (urbi_event_id_t)base;
        r.has_payload = base == 1;
        r.payload = ((uint64_t)base << 32) | i;
        while (base == 1 ? port_queue_post(&r) != 0 : port_queue_post_from_isr(&r) != 0) { /* full: retry, the port never blocks */ }
    }
    return NULL;
}
static void test_only_the_drainer_injects_and_nothing_is_lost(void)
{
    mock_freertos_reset();
    assert(port_queue_init(64) == 0);
    pthread_t a, b;
    pthread_create(&a, NULL, producer, (void *)1);
    pthread_create(&b, NULL, producer, (void *)2);
    size_t drained = 0;
    while (drained < 2 * PER_PRODUCER) {
        if (port_queue_wait(10) == 1 || 1) drained += port_queue_drain(NULL);
    }
    pthread_join(a, NULL); pthread_join(b, NULL);
    drained += port_queue_drain(NULL);
    assert(drained == 2 * PER_PRODUCER);
    assert(mock_inject_count == 2 * PER_PRODUCER);
    unsigned next1 = 0, next2 = 0;
    for (size_t i = 0; i < mock_inject_count; i++) {
        assert(pthread_equal(mock_injects[i].thread, pthread_self()));   /* the single producer */
        unsigned seq = (unsigned)(mock_injects[i].payload & 0xFFFFFFFFu);
        if (mock_injects[i].id == 1) { assert(mock_injects[i].n == 8); assert(seq == next1++); }
        else                         { assert(mock_injects[i].n == 0); assert(seq == next2++); }
    }
    printf("test_only_the_drainer_injects_and_nothing_is_lost PASS\n");
}
/* A full queue refuses the post, counts the drop, and the poster is not
 * blocked. */
static void test_queue_full_drops_and_counts_without_blocking(void)
{
    mock_freertos_reset();
    assert(port_queue_init(4) == 0);
    port_event_rec r; memset(&r, 0, sizeof r); r.id = 7;
    for (int i = 0; i < 4; i++) assert(port_queue_post(&r) == 0);
    uint32_t before = port_queue_dropped();
    assert(port_queue_post(&r) == -1);
    assert(port_queue_post_from_isr(&r) == -1);
    assert(port_queue_dropped() == before + 2);
    assert(port_queue_drain(NULL) == 4);
    assert(port_queue_post(&r) == 0);                 /* room again */
    printf("test_queue_full_drops_and_counts_without_blocking PASS\n");
}
int main(void)
{
    test_only_the_drainer_injects_and_nothing_is_lost();
    test_queue_full_drops_and_counts_without_blocking();
    return 0;
}
