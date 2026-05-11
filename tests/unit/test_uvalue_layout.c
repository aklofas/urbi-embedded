/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_uvalue_layout.c — Wave 1 T6: UValue runtime layout invariants
 *
 * Runtime mirror of the compile-time _Static_assert pins in
 * include/urbi/types.h (URBI_API_PIN_LAYOUT). Catches drift in the
 * 16-byte UValue ABI contract: kind at offset 0, payload union at
 * offset 8, total size 16 bytes. */

#include "utest.h"
#include "urbi/types.h"
#include <stddef.h>

#define UTEST(name) static void name(void)

UTEST(uvalue_size_is_16) {
    UASSERT_EQ((int)sizeof(UValue), 16);
}

UTEST(uvalue_v_offset_is_8) {
    UASSERT_EQ((int)offsetof(UValue, v), 8);
}

UTEST(uvalue_kind_offset_is_0) {
    UASSERT_EQ((int)offsetof(UValue, kind), 0);
}

void test_uvalue_layout_suite(void) {
    utest_run("uvalue_size_is_16", uvalue_size_is_16);
    utest_run("uvalue_v_offset_is_8", uvalue_v_offset_is_8);
    utest_run("uvalue_kind_offset_is_0", uvalue_kind_offset_is_0);
}
