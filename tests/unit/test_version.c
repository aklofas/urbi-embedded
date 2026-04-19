/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"
#include "urbi.h"

UTEST(version_is_nonempty) {
    const char *v = urbi_version();
    UASSERT(v != NULL);
    UASSERT(v[0] != '\0');
}

UTEST(version_starts_with_zero) {
    const char *v = urbi_version();
    UASSERT_EQ(v[0], '0');
}

UTEST(version_contains_skeleton_suffix) {
    const char *v = urbi_version();
    const char *found = strstr(v, "skeleton");
    UASSERT(found != NULL);
}
