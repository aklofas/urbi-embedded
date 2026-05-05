/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"
#include "urbi/urbi.h"
#include "umodule.h"

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
    const char *v = urbi_version();
    UASSERT(strstr(v, "concurrency") != NULL);
}

static void urbi_bytecode_version_byte_is_v1_4(void) {
    UASSERT_EQ((unsigned)URBI_BYTECODE_VERSION_BYTE, 0x14u);
    UASSERT_EQ((unsigned)URBI_BYTECODE_VERSION_MAJOR, 1u);
    UASSERT_EQ((unsigned)URBI_BYTECODE_VERSION_MINOR, 4u);
}

void test_version_suite(void) {
    utest_run("version_is_nonempty", version_is_nonempty);
    utest_run("version_starts_with_zero", version_starts_with_zero);
    utest_run("version_contains_milestone_suffix", version_contains_milestone_suffix);
    utest_run("urbi_bytecode_version_byte_is_v1_4",
              urbi_bytecode_version_byte_is_v1_4);
}
