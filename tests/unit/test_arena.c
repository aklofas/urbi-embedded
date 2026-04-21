/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for the compiler arena allocator. */

#include "utest.h"
#include "uarena.h"
#include <stdint.h>

#define UTEST(name) static void name(void)

UTEST(arena_init_does_not_allocate) {
    Arena a;
    uarena_init(&a, 0);
    UASSERT(a.first == NULL);
    UASSERT(a.head == NULL);
    UASSERT(a.oom == false);
    UASSERT(a.is_static == false);
    uarena_destroy(&a);
}

UTEST(arena_basic_alloc_returns_non_null) {
    Arena a;
    uarena_init(&a, 0);
    void *p = uarena_alloc(&a, 16);
    UASSERT(p != NULL);
    UASSERT(a.oom == false);
    uarena_destroy(&a);
}

UTEST(arena_alloc_is_zero_filled) {
    Arena a;
    uarena_init(&a, 0);
    unsigned char *p = uarena_alloc(&a, 32);
    UASSERT(p != NULL);
    for (int i = 0; i < 32; i++) {
        UASSERT_EQ(p[i], 0);
    }
    uarena_destroy(&a);
}

UTEST(arena_alloc_is_max_aligned) {
    Arena a;
    uarena_init(&a, 0);
    /* Ask for an odd size first so subsequent alloc must re-align. */
    (void)uarena_alloc(&a, 1);
    void *p = uarena_alloc(&a, 16);
    UASSERT(p != NULL);
    uintptr_t addr = (uintptr_t)p;
    /* max_align_t is typically 8 on 32-bit, 16 on 64-bit; require >= 8. */
    UASSERT_EQ(addr % 8, 0);
    uarena_destroy(&a);
}

UTEST(arena_custom_chunk_size_respected) {
    Arena a;
    uarena_init(&a, 512);
    UASSERT_EQ(a.chunk_size, 512);
    void *p = uarena_alloc(&a, 64);
    UASSERT(p != NULL);
    uarena_destroy(&a);
}

UTEST(arena_destroy_is_safe_on_empty) {
    Arena a;
    uarena_init(&a, 0);
    uarena_destroy(&a);
    /* Should not crash; nothing to assert beyond that. */
    UASSERT(1);
}

void test_arena_suite(void) {
    utest_run("arena_init_does_not_allocate",       arena_init_does_not_allocate);
    utest_run("arena_basic_alloc_returns_non_null", arena_basic_alloc_returns_non_null);
    utest_run("arena_alloc_is_zero_filled",         arena_alloc_is_zero_filled);
    utest_run("arena_alloc_is_max_aligned",         arena_alloc_is_max_aligned);
    utest_run("arena_custom_chunk_size_respected",  arena_custom_chunk_size_respected);
    utest_run("arena_destroy_is_safe_on_empty",     arena_destroy_is_safe_on_empty);
}
