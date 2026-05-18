/* SPDX-License-Identifier: BSD-3-Clause */
#include "mock_bsp.h"
#include "../../components/stm32f4-hal-baremetal/include/port_stm32f4.h"
#include <stdio.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
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

/* v0.8.2 freelist tests. */

extern size_t port_alloc_freelist_hits(void);
extern size_t port_alloc_count(void);
extern size_t port_alloc_free_count(void);
extern size_t port_alloc_heap_top(void);

static void test_free_then_alloc_reuses_freelist(void) {
    size_t hits_before = port_alloc_freelist_hits();
    void *p = port_alloc(NULL, 1024, NULL);
    assert(p != NULL);
    memset(p, 0xAA, 1024);
    port_alloc(p, 0, NULL);                  /* free -> push */
    void *q = port_alloc(NULL, 1024, NULL);  /* exact size -> reuse */
    assert(q == p);                          /* LIFO -> same block */
    assert(port_alloc_freelist_hits() == hits_before + 1);
    port_alloc(q, 0, NULL);
    printf("test_free_then_alloc_reuses_freelist PASS\n");
}

static void test_oversize_freelist_block_is_split(void) {
    /* Free a 4 KB block, then ask for 256 B; the 4 KB block should be
     * split, the 256 B chunk handed back, and the ~3.75 KB tail returned
     * to the freelist for a subsequent alloc.  The next alloc of any
     * size <= the tail's payload must come from the freelist, not bump. */
    void *big = port_alloc(NULL, 4096, NULL);
    assert(big);
    port_alloc(big, 0, NULL);
    size_t hits_before = port_alloc_freelist_hits();
    void *small = port_alloc(NULL, 256, NULL);
    assert(small != NULL);
    assert(port_alloc_freelist_hits() == hits_before + 1);
    size_t top_before = port_alloc_heap_top();
    void *tail_consumer = port_alloc(NULL, 1024, NULL);
    assert(tail_consumer != NULL);
    assert(port_alloc_heap_top() == top_before);  /* came from split tail */
    port_alloc(small, 0, NULL);
    port_alloc(tail_consumer, 0, NULL);
    printf("test_oversize_freelist_block_is_split PASS\n");
}

static void test_realloc_grow_preserves_contents(void) {
    char *p = port_alloc(NULL, 32, NULL);
    assert(p != NULL);
    for (int i = 0; i < 32; i++) p[i] = (char)i;
    char *q = port_alloc(p, 256, NULL);
    assert(q != NULL);
    for (int i = 0; i < 32; i++) assert(q[i] == (char)i);
    port_alloc(q, 0, NULL);
    printf("test_realloc_grow_preserves_contents PASS\n");
}

int main(void) {
    test_alloc_basic();
    test_realloc_shrink();
    test_alloc_oom();
    test_free_then_alloc_reuses_freelist();
    test_oversize_freelist_block_is_split();
    test_realloc_grow_preserves_contents();
    return 0;
}
