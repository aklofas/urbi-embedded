/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_object_reflection.c — Object reflection natives.
 *
 * Covers the proto-chain reach of slotNames (awkward in a .chk fixture)
 * and the nil/empty contract of getProperty / properties on a slot with
 * no installed property.  localSlotNames / hasLocalSlot get their primary
 * coverage in tests/chk/objects/reflection.chk. */

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "utest_e2e_helpers.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* slotNames spans self's local slots AND each prototype's local slots.
 * `base` carries one slot (b); a clone of base gains a local slot (a);
 * slotNames on the clone therefore reports 2. */
UTEST(slotnames_spans_proto_chain)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    UValue out;
    int rc = utest_e2e_compile_and_run(&vm,
        "var base = Object.clone(); base.setSlot(\"b\", 1); "
        "var d = base.clone(); d.setSlot(\"a\", 2); "
        "d.slotNames().length()", &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ(out.kind, (uint8_t)UVAL_INT);
    UASSERT_EQ(out.v.i, (int64_t)2);
    urbi_vm_destroy(&vm);
}

/* localSlotNames stops at the receiver's own shape — the inherited `b`
 * is not reported, only the local `a`. */
UTEST(localslotnames_excludes_proto)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    UValue out;
    int rc = utest_e2e_compile_and_run(&vm,
        "var base = Object.clone(); base.setSlot(\"b\", 1); "
        "var d = base.clone(); d.setSlot(\"a\", 2); "
        "d.localSlotNames().length()", &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ(out.kind, (uint8_t)UVAL_INT);
    UASSERT_EQ(out.v.i, (int64_t)1);
    urbi_vm_destroy(&vm);
}

/* getProperty on a slot with no installed property returns nil. */
UTEST(getproperty_unset_returns_nil)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    UValue out;
    int rc = utest_e2e_compile_and_run(&vm,
        "var o = Object.clone(); o.setSlot(\"v\", 1); "
        "o.getProperty(\"v\", \"oget\")", &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ(out.kind, (uint8_t)UVAL_NIL);
    urbi_vm_destroy(&vm);
}

/* getProperty on a missing slot returns nil (not an error). */
UTEST(getproperty_missing_slot_returns_nil)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));
    UValue out;
    int rc = utest_e2e_compile_and_run(&vm,
        "Object.clone().getProperty(\"nope\", \"oget\")", &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ(out.kind, (uint8_t)UVAL_NIL);
    urbi_vm_destroy(&vm);
}

void test_object_reflection_suite(void);
void test_object_reflection_suite(void)
{
    printf("test_object_reflection\n");
    utest_run("slotnames_spans_proto_chain",       slotnames_spans_proto_chain);
    utest_run("localslotnames_excludes_proto",      localslotnames_excludes_proto);
    utest_run("getproperty_unset_returns_nil",       getproperty_unset_returns_nil);
    utest_run("getproperty_missing_slot_returns_nil", getproperty_missing_slot_returns_nil);
}
