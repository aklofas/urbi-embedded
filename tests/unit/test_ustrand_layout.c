/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: UStrand layout additions from spec #1 §4.2 and spec #2 §3.
 *
 * Checks:
 *   - watcher_body_owner field is present and pointer-sized (spec #1 §4.2).
 *   - USTRAND_WAIT_WATCHER == 0x32u (spec #2 §3).
 *   - sizeof(UStrand) >= 264 (M3 256 B baseline + 8 B new pointer field). */

#include "utest.h"
#include "ustrand.h"

static void ustrand_spec1_field(void)
{
    UStrand s = {0};
    /* Compile-time field presence check: assignment must compile. */
    s.watcher_body_owner = NULL;   /* spec #1 §4.2 */
    /* spec #2 §3 — waituntil(cond) parked state */
    UASSERT_EQ(0x32u, USTRAND_WAIT_WATCHER);
    /* M3 baseline was 256 B; adding one pointer field grows it by 8 B. */
    UASSERT(sizeof(UStrand) >= 264u);
    (void)s;
}

static void ustrand_spec3_fields(void)
{
    UStrand s = {0};
    s.next_event_waiter = NULL;
    s.wait_event_target = NULL;
    s.last_event_payload = (UValue){0};
    UASSERT_EQ(0x33u, USTRAND_WAIT_EVENT);
    UASSERT(sizeof(UStrand) >= 288u);  /* spec #3 §3.3 default */
    (void)s;
}

void
test_ustrand_layout_suite(void)
{
    printf("test_ustrand_layout\n");
    utest_run("ustrand_spec1_field", ustrand_spec1_field);
    utest_run("ustrand_spec3_fields", ustrand_spec3_fields);
}
