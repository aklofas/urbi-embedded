/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: UCleanupEntry struct layout, push/pop LIFO semantics, overflow. */

#include "utest.h"
#include "runtime/ucleanup.h"
#include "sched/ustrand.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

/* Case 1: sizeof(UCleanupEntry) is 48 bytes on a 64-bit host.
   Layout: 10 B fixed header (kind:1 + flags:1 + register_base:2 +
           register_count:2 + handler_pc:2 + frame_depth:2) + 6 B padding
           + 4 pointers × 8 B = 48 B (refactor-3 VM-01 added frame_depth).
   On 32-bit MCU (Cortex-M, rv32): 10 B fixed + 2 B pad + 4 × 4 B = 28 B.
   Tests run host-side only, so 48 B is the operative assertion. */
UTEST(cleanup_size_48_bytes) {
    UASSERT_EQ(sizeof(UCleanupEntry), 48U);
}

/* Case 2: push two entries, verify LIFO ordering and depth tracking, pop both. */
UTEST(cleanup_push_pop_basic) {
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);

    UCleanupEntry *e1 = strand_cleanup_push(&s);
    UASSERT(e1 != NULL);
    UASSERT_EQ((unsigned)s.cleanup_depth, 1U);
    e1->kind  = (uint8_t)UCLEANUP_TRY_FRAME;
    e1->flags = FLAG_HAS_FINALLY;

    UCleanupEntry *e2 = strand_cleanup_push(&s);
    UASSERT(e2 != NULL);
    UASSERT_EQ((unsigned)s.cleanup_depth, 2U);
    e2->kind = (uint8_t)UCLEANUP_TAG_SCOPE;

    /* LIFO: pop TAG_SCOPE first, then TRY_FRAME. */
    strand_cleanup_pop(&s, UCLEANUP_TAG_SCOPE);
    UASSERT_EQ((unsigned)s.cleanup_depth, 1U);
    UASSERT(s.cleanup_top == e1);

    strand_cleanup_pop(&s, UCLEANUP_TRY_FRAME);
    UASSERT_EQ((unsigned)s.cleanup_depth, 0U);
    UASSERT(s.cleanup_top == NULL);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* Case 3: fill stack to URBI_CLEANUP_MAX, then verify one more push returns NULL. */
UTEST(cleanup_overflow_returns_null) {
    UVM vm;
    UStrand s;
    uint16_t i;
    urbi_vm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);

    for (i = 0; i < (uint16_t)URBI_CLEANUP_MAX; i++) {
        UCleanupEntry *e = strand_cleanup_push(&s);
        UASSERT(e != NULL);
    }
    UASSERT_EQ((unsigned)s.cleanup_depth, (unsigned)URBI_CLEANUP_MAX);

    /* One push beyond capacity must return NULL. */
    UCleanupEntry *overflow = strand_cleanup_push(&s);
    UASSERT(overflow == NULL);
    /* Depth must not have advanced past the cap. */
    UASSERT_EQ((unsigned)s.cleanup_depth, (unsigned)URBI_CLEANUP_MAX);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

void test_cleanup_suite(void) {
    utest_run("cleanup_size_48_bytes",      cleanup_size_48_bytes);
    utest_run("cleanup_push_pop_basic",     cleanup_push_pop_basic);
    utest_run("cleanup_overflow_returns_null", cleanup_overflow_returns_null);
}
