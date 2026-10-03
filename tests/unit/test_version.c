/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"
#include "urbi/urbi.h"
#include "chunk/uchunk.h"

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

static void urbi_bytecode_version_byte_is_v2_0(void) {
    /* Wire v2 renumbered the opcode set to 41 rows (EXTARG site
     * widening, the scope / fork / install folds), so the format is
     * v2.0 / 0x20 and every v1.x blob is rejected at the version byte. */
    UASSERT_EQ((unsigned)URBI_BYTECODE_VERSION_BYTE, 0x20U);
    UASSERT_EQ((unsigned)URBI_BYTECODE_VERSION_MAJOR, 2U);
    UASSERT_EQ((unsigned)URBI_BYTECODE_VERSION_MINOR, 0U);
    UASSERT_EQ((int)OP_MAX, 41);
}

void test_version_suite(void) {
    utest_run("version_is_nonempty", version_is_nonempty);
    utest_run("version_starts_with_zero", version_starts_with_zero);
    utest_run("version_contains_milestone_suffix", version_contains_milestone_suffix);
    utest_run("urbi_bytecode_version_byte_is_v2_0",
              urbi_bytecode_version_byte_is_v2_0);
}
