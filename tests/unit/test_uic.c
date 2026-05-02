/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for src/object/uic.h — UIC inline-cache record + tunable.
 *
 * The header carries a gated _Static_assert pinning sizeof(UIC) == 144 at
 * the default 4-entry / 64-bit-pointer build; if it trips this file won't
 * compile.  These runtime tests give a second, test-runner-visible signal
 * and additionally re-pin the URBI_SLOT_FLAG_* attribute bits for the
 * IC.flags summary defined alongside in uobject.h. */

#include "utest.h"

#include "object/uic.h"
#include "object/uobject.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

UTEST(uic_layout_at_default_4_entries) {
    UASSERT_EQ(URBI_IC_ENTRIES_PER_SITE, 4);
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
    UASSERT_EQ((int)sizeof(UIC), 144);
#endif
}

UTEST(uic_flag_bits_distinct) {
    UASSERT_EQ((int)(URBI_SLOT_FLAG_OGET
                   | URBI_SLOT_FLAG_OSET
                   | URBI_SLOT_FLAG_CONSTANT
                   | URBI_SLOT_FLAG_LOCAL),
               0x0F);
}

void test_uic_suite(void) {
    utest_run("uic: layout at default 4 entries",
              uic_layout_at_default_4_entries);
    utest_run("uic: flag bits distinct",
              uic_flag_bits_distinct);
}
