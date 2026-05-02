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

/* === Slot-flag + atom-family bit patterns === */

UTEST(uobject_atom_mask_and_flag_bits_are_distinct) {
    /* Low nibble of UObject.flags reserved for atom family; bit 4/5
     * carry frozen + sandbox-readonly. */
    UASSERT_EQ(URBI_OBJ_ATOM_MASK,       0x0Fu);
    UASSERT_EQ(URBI_OBJ_FLAG_FROZEN,     0x10u);
    UASSERT_EQ(URBI_OBJ_FLAG_SANDBOX_RO, 0x20u);
    /* The four slot-property flags occupy bits 0..3 with no overlap. */
    UASSERT_EQ(URBI_SLOT_FLAG_OGET     |
               URBI_SLOT_FLAG_OSET     |
               URBI_SLOT_FLAG_CONSTANT |
               URBI_SLOT_FLAG_LOCAL,    0x0Fu);
}

UTEST(uobject_atom_family_values_pinned) {
    /* Load-bearing for T8 atom-singleton install order; do not renumber. */
    UASSERT_EQ(URBI_ATOM_OBJECT,  0);
    UASSERT_EQ(URBI_ATOM_INTEGER, 1);
    UASSERT_EQ(URBI_ATOM_FLOAT,   2);
    UASSERT_EQ(URBI_ATOM_STRING,  3);
    UASSERT_EQ(URBI_ATOM_LIST,    4);
    UASSERT_EQ(URBI_ATOM_DICT,    5);
    UASSERT_EQ(URBI_ATOM_TAG,     6);
    UASSERT_EQ(URBI_ATOM_EVENT,   7);
    UASSERT_EQ(URBI_ATOM_SYMBOL,  8);
}

void test_uobject_suite(void) {
    utest_run("uobject: USlot == UValue (16 B)", uobject_uslot_is_exactly_uvalue);
    utest_run("uobject: header is 48 bytes", uobject_header_is_48_bytes);
    utest_run("uobject: field order matches spec §3", uobject_field_order_matches_spec);
    utest_run("uobject: atom mask + flag bits distinct", uobject_atom_mask_and_flag_bits_are_distinct);
    utest_run("uobject: atom family values pinned", uobject_atom_family_values_pinned);
}
