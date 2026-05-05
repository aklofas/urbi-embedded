/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: gc_byte bit allocation completeness.
 *
 * Verifies that UGC_HAS_SLOT_CHANGE_EVENT is defined as bit 7 (0x80),
 * and that all 8 gc_byte bits are accounted for with no collisions.
 * Spec #4 §3.4.
 */

#include "utest.h"
#include "gc/ugc_incremental.h"
#include <stdint.h>

#define UTEST(name) static void name(void)

/* ===== Test 1: UGC_HAS_SLOT_CHANGE_EVENT is bit 7 ===== */

UTEST(gc_byte_bit7_allocated)
{
    UASSERT_EQ(UGC_HAS_SLOT_CHANGE_EVENT, 0x80);
}

/* ===== Test 2: all 8 gc_byte bits accounted for, no collisions ===== */
/*
 * Bit layout (ugc_incremental.h):
 *   [1:0] = UGC_COLOR_MASK  (0x03, covers two bits for tri-color)
 *   [2]   = UGC_HAS_FINALIZER        (0x04)
 *   [3]   = UGC_IS_WEAK              (0x08, RESERVED v1.x but allocated)
 *   [4]   = UGC_IS_PINNED            (0x10)
 *   [5]   = UGC_IS_FIXED             (0x20)
 *   [6]   = UGC_HAS_WATCHER_OBSERVER (0x40)
 *   [7]   = UGC_HAS_SLOT_CHANGE_EVENT(0x80) — allocated here (spec #4 §3.4)
 *
 * The OR of all bit-mask constants must equal 0xFF.
 * UGC_COLOR_MASK covers bits 0+1; the remaining six constants each claim one bit.
 * After this task, all 8 bits are claimed; future feature additions must
 * multiplex an existing bit or extend gc_byte to gc_word.
 */

UTEST(gc_byte_no_collisions)
{
    uint8_t all = (uint8_t)(
        UGC_COLOR_MASK           /* 0x03 — bits [1:0] */
        | UGC_HAS_FINALIZER      /* 0x04 — bit 2 */
        | UGC_IS_WEAK            /* 0x08 — bit 3 (reserved v1.x) */
        | UGC_IS_PINNED          /* 0x10 — bit 4 */
        | UGC_IS_FIXED           /* 0x20 — bit 5 */
        | UGC_HAS_WATCHER_OBSERVER   /* 0x40 — bit 6 */
        | UGC_HAS_SLOT_CHANGE_EVENT  /* 0x80 — bit 7 */
    );
    UASSERT_EQ(all, 0xFF);
}

/* ===== Suite entry point ===== */

void test_gc_byte_suite(void)
{
    utest_run("gc_byte_bit7_allocated",  gc_byte_bit7_allocated);
    utest_run("gc_byte_no_collisions",   gc_byte_no_collisions);
}
