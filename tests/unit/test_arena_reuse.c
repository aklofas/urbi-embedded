/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for arena chunk reuse after uarena_reset (refactor-3 FE-07).
 *
 * UArenaChunk is private to uarena.c, so the chunk chain is observed
 * indirectly through a counting backing allocator (the same AllocSpy
 * pattern test_arena.c uses): every chunk the arena creates is one
 * spy_alloc call, every chunk uarena_destroy releases is one spy_free
 * call.  Pre-fix, the reset-then-regrow sequence appended the new chunk
 * directly at head, clobbering head->next and orphaning every chunk
 * behind it — visible here as alloc_calls growing past the first round's
 * count AND free_calls < alloc_calls at destroy (a genuine malloc leak;
 * LSan flags it under make test-asan). */

#include "utest.h"
#include "value/uarena.h"

#include <stdlib.h>

#define UTEST(name) static void name(void)

typedef struct {
    int alloc_calls;
    int free_calls;
} AllocSpy;

static void *spy_alloc(size_t n, void *ud) {
    AllocSpy *s = ud;
    s->alloc_calls++;
    return malloc(n);
}

static void spy_free(void *p, void *ud) {
    AllocSpy *s = ud;
    s->free_calls++;
    free(p);
}

UTEST(reset_then_grow_does_not_orphan) {
    AllocSpy s = { 0, 0 };
    UArena a;
    uarena_init_ex(&a, 256, spy_alloc, spy_free, &s);   /* small chunks force growth */

    /* Fill chunk 1 and force chunks 2..N into existence (each 200-byte
     * request rounds to 208; a 256-payload chunk holds exactly one). */
    for (int i = 0; i < 8; i++) {
        UASSERT(uarena_alloc(&a, 200) != NULL);
    }
    int chunks_before = s.alloc_calls;
    UASSERT(chunks_before >= 2);

    uarena_reset(&a);

    /* Allocate enough to overflow chunk 1 again: pre-fix, the new-chunk
     * append clobbered first->next and orphaned (leaked) chunks 2..N. */
    for (int i = 0; i < 8; i++) {
        UASSERT(uarena_alloc(&a, 200) != NULL);
    }
    int chunks_after = s.alloc_calls;

    /* Post-fix: the chain reuses the existing chunks; no growth needed. */
    UASSERT_EQ(chunks_after, chunks_before);

    uarena_destroy(&a);
    /* Every chunk ever created must be reachable at destroy time. */
    UASSERT_EQ(s.free_calls, s.alloc_calls);
}

UTEST(reset_mixed_size_insert_keeps_chain_reachable) {
    AllocSpy s = { 0, 0 };
    UArena a;
    uarena_init_ex(&a, 256, spy_alloc, spy_free, &s);

    /* Build chunk 1 + chunk 2. */
    UASSERT(uarena_alloc(&a, 200) != NULL);
    UASSERT(uarena_alloc(&a, 200) != NULL);
    UASSERT_EQ(s.alloc_calls, 2);

    uarena_reset(&a);

    /* Small alloc reuses chunk 1. */
    UASSERT(uarena_alloc(&a, 200) != NULL);
    UASSERT_EQ(s.alloc_calls, 2);

    /* Oversized alloc does NOT fit chunk 2's capacity — a new big chunk
     * must be inserted into the chain (between chunk 1 and chunk 2 in the
     * fixed code) without dropping chunk 2. */
    UASSERT(uarena_alloc(&a, 1024) != NULL);
    UASSERT_EQ(s.alloc_calls, 3);

    /* Next small alloc: the big chunk is full, so the walk must find
     * chunk 2 (still linked, used == 0) and reuse it — no growth. */
    UASSERT(uarena_alloc(&a, 200) != NULL);
    UASSERT_EQ(s.alloc_calls, 3);

    uarena_destroy(&a);
    UASSERT_EQ(s.free_calls, s.alloc_calls);
}

void test_arena_reuse_suite(void) {
    utest_run("reset_then_grow_does_not_orphan",
              reset_then_grow_does_not_orphan);
    utest_run("reset_mixed_size_insert_keeps_chain_reachable",
              reset_mixed_size_insert_keeps_chain_reachable);
}
