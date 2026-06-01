/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_api_version.c — Wave 1 T5: ABI version macros + getter */

#include "utest.h"
#include "urbi/version.h"

#define UTEST(name) static void name(void)

UTEST(api_version_macros_are_defined) {
    UASSERT_EQ(URBI_API_VERSION_MAJOR, 0);
    UASSERT_EQ(URBI_API_VERSION_MINOR, 22);
    UASSERT_EQ(URBI_API_VERSION_PATCH, 0);
    UASSERT_EQ(URBI_API_VERSION_NUM, 2200);
}

UTEST(api_version_getter_returns_macros) {
    int major = -99, minor = -99, patch = -99;
    urbi_api_version(&major, &minor, &patch);
    UASSERT_EQ(major, URBI_API_VERSION_MAJOR);
    UASSERT_EQ(minor, URBI_API_VERSION_MINOR);
    UASSERT_EQ(patch, URBI_API_VERSION_PATCH);
}

UTEST(api_version_getter_null_tolerant) {
    int x = -99;
    urbi_api_version(NULL, NULL, NULL);  /* must not crash */
    urbi_api_version(&x, NULL, NULL);
    UASSERT_EQ(x, URBI_API_VERSION_MAJOR);
}

void test_api_version_suite(void) {
    utest_run("api_version_macros_are_defined", api_version_macros_are_defined);
    utest_run("api_version_getter_returns_macros", api_version_getter_returns_macros);
    utest_run("api_version_getter_null_tolerant", api_version_getter_null_tolerant);
}
