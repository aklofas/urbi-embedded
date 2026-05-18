/* SPDX-License-Identifier: BSD-3-Clause */
#include "mock_bsp.h"
#include "../../components/stm32f4-hal-baremetal/include/port_stm32f4.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

static void test_alloc_basic(void) {
    void *p = port_alloc(NULL, 64, NULL);
    assert(p != NULL);
    memset(p, 0xAA, 64);  /* writable */
    port_alloc(p, 0, NULL);  /* free */
    printf("test_alloc_basic PASS\n");
}

static void test_realloc_shrink(void) {
    void *p = port_alloc(NULL, 256, NULL);
    assert(p);
    void *q = port_alloc(p, 64, NULL);
    assert(q);
    port_alloc(q, 0, NULL);
    printf("test_realloc_shrink PASS\n");
}

static void test_alloc_oom(void) {
    /* The static heap is URBI_HEAP_BYTES (80 KB default). Allocating
     * something far larger MUST return NULL, not crash. */
    void *p = port_alloc(NULL, 1024 * 1024, NULL);
    assert(p == NULL);
    printf("test_alloc_oom PASS\n");
}

int main(void) {
    test_alloc_basic();
    test_realloc_shrink();
    test_alloc_oom();
    return 0;
}
