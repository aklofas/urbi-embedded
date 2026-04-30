/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for src/object/uobject.h — UObject/UProtos/USlot layouts.
 *
 * The header itself carries _Static_assert pins on UObject and USlot widths;
 * if those trip, this file won't compile.  These runtime tests give a second,
 * test-runner-visible signal that the layout is what the spec says it is. */

#include "utest.h"

#include "object/uobject.h"
#include "umodule.h"   /* UValue */

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* === USlot width === */

UTEST(uobject_uslot_is_exactly_uvalue) {
    /* USlot collapses to UValue per pre-M4 USlot/UProps spec §3. */
    UASSERT_EQ((int)sizeof(USlot), (int)sizeof(UValue));
    UASSERT_EQ((int)sizeof(USlot), 16);
}

/* === UObject header width === */

UTEST(uobject_header_is_48_bytes) {
    /* Pinned by spec §3 (pre-M4 prototype-chain representation design). */
    UASSERT_EQ((int)sizeof(UObject), 48);
}

/* === UObject field offsets ===
 * Field order matters: layout must be exactly cell -> shape -> slots ->
 * protos -> object_id -> lookup_stamp -> flags -> reserved. */

UTEST(uobject_field_order_matches_spec) {
    /* cell at offset 0. */
    UASSERT_EQ((int)offsetof(UObject, cell), 0);
    /* shape at offset 8 (UCell + 6 B compiler-inserted padding). */
    UASSERT_EQ((int)offsetof(UObject, shape), 8);
    /* slots, protos at 16, 24. */
    UASSERT_EQ((int)offsetof(UObject, slots), 16);
    UASSERT_EQ((int)offsetof(UObject, protos), 24);
    /* object_id, lookup_stamp, flags, reserved at 32, 36, 40, 44. */
    UASSERT_EQ((int)offsetof(UObject, object_id), 32);
    UASSERT_EQ((int)offsetof(UObject, lookup_stamp), 36);
    UASSERT_EQ((int)offsetof(UObject, flags), 40);
    UASSERT_EQ((int)offsetof(UObject, reserved), 44);
}

void test_uobject_suite(void) {
    utest_run("uobject: USlot == UValue (16 B)", uobject_uslot_is_exactly_uvalue);
    utest_run("uobject: header is 48 bytes", uobject_header_is_48_bytes);
    utest_run("uobject: field order matches spec §3", uobject_field_order_matches_spec);
}
