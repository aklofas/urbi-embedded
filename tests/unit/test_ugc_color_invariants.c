/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: UCell gc_byte color invariants + two-white scheme + alloc
 * debt trigger.  Row 10 §3.1–§3.5.  T23 baseline. */

#include "utest.h"
#include "ugc_capi.h"
#include "ugc_incremental.h"
#include "uvm.h"
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* ===== Test 1: two-white flip ===== */

/* After uvm_init the current_white is 0; manually flipping it yields 1;
 * OTHER_WHITE then yields 0 again.  T24 will flip current_white at the
 * IDLE → MARK_ROOTS transition; this test validates the macro arithmetic. */
UTEST(ugc_two_white_flip)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UASSERT_EQ(vm.current_white, 0u);

    /* Manually flip — T24 does this at the IDLE → MARK_ROOTS transition. */
    vm.current_white ^= 0x01u;
    UASSERT_EQ(vm.current_white, 1u);
    UASSERT_EQ(OTHER_WHITE(&vm), 0u);

    uvm_destroy(&vm);
}

/* ===== Test 2: alloc paints cell current_white ===== */

/* A freshly allocated cell must have its color bits equal to current_white
 * (spec §3.5: cells born during a GC cycle are painted current_white). */
UTEST(ugc_alloc_born_current_white)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UCell *c = urbi_gc_alloc(&vm, sizeof(UCell) + 32u, UTYPE_OBJECT);
    UASSERT(c != NULL);
    UASSERT_EQ((c->gc_byte & UGC_COLOR_MASK), vm.current_white);
    UASSERT_EQ(c->type_tag, (uint8_t)UTYPE_OBJECT);

    uvm_destroy(&vm);
}

/* ===== Test 3: dead color after white flip ===== */

/* After allocating a cell (color = current_white = 0) then flipping
 * current_white to 1, IS_DEAD should return true for that cell because
 * its color matches the "other" (previous) white. */
UTEST(ugc_dead_color_after_flip)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UCell *c = urbi_gc_alloc(&vm, sizeof(UCell) + 32u, UTYPE_OBJECT);
    UASSERT(c != NULL);

    /* c was born white(0); flip current_white → c is now in OTHER_WHITE. */
    vm.current_white ^= 0x01u;
    UASSERT(IS_DEAD(&vm, c));

    uvm_destroy(&vm);
}

/* ===== Test 4: alloc increments gc_debt ===== */

/* Every urbi_gc_alloc call must increase gc_debt by the requested size.
 * (gc_debt starts at -URBI_GC_INITIAL_THRESHOLD; it rises toward zero and
 * eventually triggers gc_pending.) */
UTEST(ugc_alloc_increments_debt)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    int64_t debt_before = vm.gc_debt;

    UCell *c = urbi_gc_alloc(&vm, sizeof(UCell) + 100u, UTYPE_OBJECT);
    UASSERT(c != NULL);
    UASSERT(vm.gc_debt > debt_before);

    uvm_destroy(&vm);
}

/* ===== Test 5: gc_pending set when debt crosses zero ===== */

/* When gc_debt is just below 0 and a large-enough allocation pushes it
 * positive (and gc_paused == 0), gc_pending must be set to 1. */
UTEST(ugc_alloc_triggers_pending_at_threshold)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* Force debt to -1: one byte of credit remaining. */
    vm.gc_debt = -1;

    /* Allocate 100 bytes — well over the remaining credit — so debt goes
     * positive.  Note: urbi_gc_alloc with option-B sidecar works for any
     * size >= sizeof(UCell) (header is inside the allocated block).
     * size=100 is fine with option B; sidecar is separate. */
    UCell *c = urbi_gc_alloc(&vm, 100u, UTYPE_OBJECT);
    UASSERT(c != NULL);
    UASSERT_EQ(vm.gc_pending, 1u);

    uvm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void test_ugc_color_invariants_suite(void)
{
    utest_run("ugc_two_white_flip",
              ugc_two_white_flip);
    utest_run("ugc_alloc_born_current_white",
              ugc_alloc_born_current_white);
    utest_run("ugc_dead_color_after_flip",
              ugc_dead_color_after_flip);
    utest_run("ugc_alloc_increments_debt",
              ugc_alloc_increments_debt);
    utest_run("ugc_alloc_triggers_pending_at_threshold",
              ugc_alloc_triggers_pending_at_threshold);
}
