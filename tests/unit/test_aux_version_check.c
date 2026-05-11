/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_aux_version_check.c — T13: urbi_aux_check_version helper */

#include "utest.h"
#include "urbi/aux.h"
#include "urbi/types.h"
#include "urbi/version.h"

#define UTEST(name) static void name(void)

UTEST(aux_check_version_matches_when_in_sync) {
    /* Test runner compiles against the same headers + library, so
     * compile-time URBI_API_VERSION_* equals the runtime urbi_api_version().
     * Mismatch is unreachable in this in-tree configuration. */
    int rc = urbi_aux_check_version();
    UASSERT_EQ(rc, URBI_OK);
}

void test_aux_version_check_suite(void) {
    utest_run("aux_check_version_matches_when_in_sync",
              aux_check_version_matches_when_in_sync);
}
