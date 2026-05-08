/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"
#include "urbi/urbi.h"
#include "module/umodule.h"

static void version_is_nonempty(void) {
    const char *v = urbi_version();
    UASSERT(v != NULL);
    UASSERT(v[0] != '\0');
}

static void version_starts_with_zero(void) {
    const char *v = urbi_version();
    UASSERT_EQ(v[0], '0');
}

static void version_contains_milestone_suffix(void) {
    /* The version literal carries a milestone or wave suffix.  Wave 5 fixes
     * the v0.5.7 carry-forward (API-011); pre-Wave-5 the suffix was the
     * stale "concurrency" inherited from M3.  Both forms include a hyphen,
     * so the assertion checks for the format rather than a fixed suffix. */
    const char *v = urbi_version();
    UASSERT(strchr(v, '-') != NULL);
}

static void urbi_bytecode_version_byte_is_v1_5(void) {
    UASSERT_EQ((unsigned)URBI_BYTECODE_VERSION_BYTE, 0x15U);
    UASSERT_EQ((unsigned)URBI_BYTECODE_VERSION_MAJOR, 1U);
    UASSERT_EQ((unsigned)URBI_BYTECODE_VERSION_MINOR, 5U);
}

void test_version_suite(void) {
    utest_run("version_is_nonempty", version_is_nonempty);
    utest_run("version_starts_with_zero", version_starts_with_zero);
    utest_run("version_contains_milestone_suffix", version_contains_milestone_suffix);
    utest_run("urbi_bytecode_version_byte_is_v1_5",
              urbi_bytecode_version_byte_is_v1_5);
}
