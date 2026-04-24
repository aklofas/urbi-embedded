/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for the compiler arena allocator. */

#include "utest.h"
#include "uarena.h"
#include <stdint.h>

#define UTEST(name) static void name(void)

UTEST(arena_init_does_not_allocate) {
    UArena a;
    uarena_init(&a, 0);
    UASSERT(a.first == NULL);
    UASSERT(a.head == NULL);
    UASSERT(a.oom == false);
    UASSERT(a.is_static == false);
    uarena_destroy(&a);
}

UTEST(arena_basic_alloc_returns_non_null) {
    UArena a;
    uarena_init(&a, 0);
    void *p = uarena_alloc(&a, 16);
    UASSERT(p != NULL);
    UASSERT(a.oom == false);
    uarena_destroy(&a);
}

UTEST(arena_alloc_is_zero_filled) {
    UArena a;
    uarena_init(&a, 0);
    unsigned char *p = uarena_alloc(&a, 32);
    UASSERT(p != NULL);
    for (int i = 0; i < 32; i++) {
        UASSERT_EQ(p[i], 0);
    }
    uarena_destroy(&a);
}

UTEST(arena_alloc_is_max_aligned) {
    UArena a;
    uarena_init(&a, 0);
    /* Ask for an odd size first so subsequent alloc must re-align. */
    (void)uarena_alloc(&a, 1);
    void *p = uarena_alloc(&a, 16);
    UASSERT(p != NULL);
    uintptr_t addr = (uintptr_t)p;
    /* Arena alignment is 16 bytes (the stricter of the two common
       max_align_t values across 32/64-bit hosts). */
    UASSERT_EQ(addr % 16, 0);
    uarena_destroy(&a);
}

UTEST(arena_custom_chunk_size_respected) {
    UArena a;
    uarena_init(&a, 512);
    UASSERT_EQ(a.chunk_size, 512);
    void *p = uarena_alloc(&a, 64);
    UASSERT(p != NULL);
    uarena_destroy(&a);
}

UTEST(arena_destroy_is_safe_on_empty) {
    UArena a;
    uarena_init(&a, 0);
    uarena_destroy(&a);
    /* Should not crash; nothing to assert beyond that. */
    UASSERT(1);
}

UTEST(arena_reset_rewinds_chunk) {
    UArena a;
    uarena_init(&a, 0);
    void *p1 = uarena_alloc(&a, 32);
    uarena_reset(&a);
    void *p2 = uarena_alloc(&a, 32);
    /* After reset the first allocation of the same size should reuse
       the same chunk payload (same address). */
    UASSERT(p1 == p2);
    uarena_destroy(&a);
}

UTEST(arena_reset_clears_oom) {
    UArena a;
    uarena_init(&a, 0);
    a.oom = true; /* simulate a prior OOM flag */
    uarena_reset(&a);
    UASSERT(a.oom == false);
    void *p = uarena_alloc(&a, 16);
    UASSERT(p != NULL);
    uarena_destroy(&a);
}

UTEST(arena_growth_preserves_earlier_pointers) {
    UArena a;
    uarena_init(&a, 256); /* small chunk forces growth */
    unsigned char *p1 = uarena_alloc(&a, 128);
    UASSERT(p1 != NULL);
    p1[0] = 0xAA;
    p1[127] = 0xBB;
    /* Exceed the remaining capacity of the first chunk. */
    unsigned char *p2 = uarena_alloc(&a, 512);
    UASSERT(p2 != NULL);
    /* p1 must remain valid and unchanged after the growth. */
    UASSERT_EQ(p1[0], 0xAA);
    UASSERT_EQ(p1[127], 0xBB);
    uarena_destroy(&a);
}

/* --- Custom allocator used by the next two tests. --- */

typedef struct {
    int alloc_calls;
    int free_calls;
    int fail_at; /* -1 means never fail */
} AllocSpy;

static void *spy_alloc(size_t n, void *ud) {
    AllocSpy *s = ud;
    s->alloc_calls++;
    if (s->fail_at >= 0 && s->alloc_calls > s->fail_at) return NULL;
    return malloc(n);
}

static void spy_free(void *p, void *ud) {
    AllocSpy *s = ud;
    s->free_calls++;
    free(p);
}

UTEST(arena_init_ex_calls_custom_alloc) {
    AllocSpy s = { 0, 0, -1 };
    UArena a;
    uarena_init_ex(&a, 0, spy_alloc, spy_free, &s);
    void *p = uarena_alloc(&a, 64);
    UASSERT(p != NULL);
    UASSERT_EQ(s.alloc_calls, 1);
    UASSERT_EQ(s.free_calls, 0);
    uarena_destroy(&a);
    UASSERT_EQ(s.free_calls, 1);
}

UTEST(arena_oom_via_failing_allocator) {
    AllocSpy s = { 0, 0, 0 }; /* fail on the very first alloc */
    UArena a;
    uarena_init_ex(&a, 0, spy_alloc, spy_free, &s);
    void *p = uarena_alloc(&a, 16);
    UASSERT(p == NULL);
    UASSERT(a.oom == true);
    /* Sticky flag: further allocs keep returning NULL. */
    void *p2 = uarena_alloc(&a, 16);
    UASSERT(p2 == NULL);
    UASSERT(a.oom == true);
    /* Reset clears the flag. */
    uarena_reset(&a);
    UASSERT(a.oom == false);
    uarena_destroy(&a);
}

UTEST(arena_init_static_does_not_call_allocator) {
    unsigned char buf[256];
    UArena a;
    uarena_init_static(&a, buf, sizeof buf);
    /* Prove the allocator fn pointers are not stored. */
    UASSERT(a.alloc_fn == NULL);
    UASSERT(a.free_fn == NULL);
    UASSERT(a.is_static == true);

    void *p = uarena_alloc(&a, 32);
    UASSERT(p != NULL);
    UASSERT(p >= (void *)buf);
    UASSERT(p < (void *)(buf + sizeof buf));
    uarena_destroy(&a);
}

UTEST(arena_static_oom_when_buffer_full) {
    unsigned char buf[128];
    UArena a;
    uarena_init_static(&a, buf, sizeof buf);
    /* One large allocation consumes the buffer. */
    void *p1 = uarena_alloc(&a, 64);
    UASSERT(p1 != NULL);
    /* A second allocation that does not fit triggers OOM (no growth
       because dynamic allocation is disabled). */
    void *p2 = uarena_alloc(&a, 128);
    UASSERT(p2 == NULL);
    UASSERT(a.oom == true);
    uarena_destroy(&a);
}

UTEST(arena_static_destroy_is_noop) {
    unsigned char buf[256];
    UArena a;
    uarena_init_static(&a, buf, sizeof buf);
    (void)uarena_alloc(&a, 64);
    uarena_destroy(&a);
    /* Caller still owns buf; writing to it must remain safe. */
    buf[0] = 0x5A;
    UASSERT_EQ(buf[0], 0x5A);
}

UTEST(arena_static_too_small_buffer_triggers_oom) {
    /* Buffer too small to even hold the chunk header + alignment.
       uarena_init_static must leave head=NULL, and the first alloc
       must OOM cleanly instead of dereferencing garbage. */
    unsigned char buf[8];
    UArena a;
    uarena_init_static(&a, buf, sizeof buf);
    UASSERT(a.head == NULL);
    UASSERT(a.is_static == true);
    UASSERT(a.oom == false); /* not yet — OOM fires on the alloc attempt */
    void *p = uarena_alloc(&a, 1);
    UASSERT(p == NULL);
    UASSERT(a.oom == true);
    uarena_destroy(&a);
}

void test_arena_suite(void) {
    utest_run("arena_init_does_not_allocate",       arena_init_does_not_allocate);
    utest_run("arena_basic_alloc_returns_non_null", arena_basic_alloc_returns_non_null);
    utest_run("arena_alloc_is_zero_filled",         arena_alloc_is_zero_filled);
    utest_run("arena_alloc_is_max_aligned",         arena_alloc_is_max_aligned);
    utest_run("arena_custom_chunk_size_respected",  arena_custom_chunk_size_respected);
    utest_run("arena_destroy_is_safe_on_empty",     arena_destroy_is_safe_on_empty);
    utest_run("arena_reset_rewinds_chunk",              arena_reset_rewinds_chunk);
    utest_run("arena_reset_clears_oom",                 arena_reset_clears_oom);
    utest_run("arena_growth_preserves_earlier_pointers", arena_growth_preserves_earlier_pointers);
    utest_run("arena_init_ex_calls_custom_alloc",  arena_init_ex_calls_custom_alloc);
    utest_run("arena_oom_via_failing_allocator",   arena_oom_via_failing_allocator);
    utest_run("arena_init_static_does_not_call_allocator", arena_init_static_does_not_call_allocator);
    utest_run("arena_static_oom_when_buffer_full",         arena_static_oom_when_buffer_full);
    utest_run("arena_static_destroy_is_noop",              arena_static_destroy_is_noop);
    utest_run("arena_static_too_small_buffer_triggers_oom", arena_static_too_small_buffer_triggers_oom);
}
