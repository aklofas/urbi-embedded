/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"
#include "urbi.h"

static void version_is_nonempty(void) {
    const char *v = urbi_version();
    UASSERT(v != NULL);
    UASSERT(v[0] != '\0');
}

static void version_starts_with_zero(void) {
    const char *v = urbi_version();
    UASSERT_EQ(v[0], '0');
}

static void version_contains_skeleton_suffix(void) {
    const char *v = urbi_version();
    UASSERT(strstr(v, "skeleton") != NULL);
}

void test_version_suite(void) {
    utest_run("version_is_nonempty", version_is_nonempty);
    utest_run("version_starts_with_zero", version_starts_with_zero);
    utest_run("version_contains_skeleton_suffix", version_contains_skeleton_suffix);
}
