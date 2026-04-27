/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: UStrand state byte encoding + transition macros. */

#include "utest.h"
#include "ustrand.h"

#define UTEST(name) static void name(void)

/* Case 1: after ustrand_init the state byte is DORMANT with reason NONE. */
UTEST(strand_state_dormant_at_init) {
    UStrand s;
    ustrand_init(&s, NULL);
    UASSERT_EQ(USTRAND_GET_STATE(&s), USTRAND_DORMANT);
    UASSERT_EQ(USTRAND_GET_REASON(&s), USTRAND_REASON_NONE);
}

/* Case 2: WAITING composite values round-trip through the helper macros. */
UTEST(strand_state_waiting_macros_round_trip) {
    UStrand s;
    ustrand_init(&s, NULL);

    s.state = USTRAND_STATE_WAITING_SLEEP;
    UASSERT(USTRAND_IS_WAITING(&s));
    UASSERT_EQ(USTRAND_GET_REASON(&s), USTRAND_REASON_SLEEP);

    s.state = USTRAND_STATE_WAITING_EVENT;
    UASSERT(USTRAND_IS_WAITING(&s));
    UASSERT_EQ(USTRAND_GET_REASON(&s), USTRAND_REASON_EVENT);

    s.state = USTRAND_STATE_WAITING_JOIN;
    UASSERT(USTRAND_IS_WAITING(&s));
    UASSERT_EQ(USTRAND_GET_REASON(&s), USTRAND_REASON_JOIN);
}

/* Case 3: RUNNING state is not flagged as WAITING; reason reads NONE. */
UTEST(strand_state_running_not_waiting) {
    UStrand s;
    ustrand_init(&s, NULL);
    s.state = USTRAND_STATE_RUNNING;
    UASSERT(!USTRAND_IS_WAITING(&s));
    UASSERT_EQ(USTRAND_GET_STATE(&s), USTRAND_RUNNING);
    UASSERT_EQ(USTRAND_GET_REASON(&s), USTRAND_REASON_NONE);
}

/* Case 4: the state field is exactly one byte wide. */
UTEST(strand_state_byte_size_one) {
    UStrand s;
    (void)s;
    UASSERT_EQ(sizeof(s.state), (size_t)1);
}

void test_strand_suite(void) {
    utest_run("strand_state_dormant_at_init",          strand_state_dormant_at_init);
    utest_run("strand_state_waiting_macros_round_trip", strand_state_waiting_macros_round_trip);
    utest_run("strand_state_running_not_waiting",       strand_state_running_not_waiting);
    utest_run("strand_state_byte_size_one",             strand_state_byte_size_one);
}
