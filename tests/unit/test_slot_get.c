/* SPDX-License-Identifier: BSD-3-Clause */
/* test_slot_get.c — TDD tests for urbi_slot_get (Gap K, v0.7.1).
 *
 * Five sub-tests:
 *   1. Get an existing slot on a UObject → URBI_OK + correct value.
 *   2. Get a slot through proto chain (set on parent, query on child).
 *   3. Get a missing slot → URBI_ERR_SLOT_NOT_FOUND.
 *   4. Get a slot on an atom (UVAL_INT) → route through Integer atom-proto.
 *   5. Get with NULL out_value → URBI_ERR_INVALID_ARG. */

#include "utest.h"

#include <stddef.h>
#include <string.h>

#include "object/uobject.h"      /* urbi_object_alloc, urbi_object_set_local_slot,
                                    urbi_atom_proto_for_value, urbi_object_atom */
#include "value/uintern.h"       /* ustr_intern */
#include "vm/uvm.h"
#include "gc/ugc.h"              /* UTYPE_OBJECT */
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "urbi/object.h"         /* URBI_ATOM_INTEGER */

#define UTEST(name) static void name(void)

/* Helper: allocate a bare UObject (no slots, root Object shape). */
static UObject *
make_object(UVM *vm)
{
    return urbi_object_alloc(vm, URBI_ATOM_OBJECT);
}

/* Helper: set a named slot on a UObject. */
static int
set_slot(UVM *vm, UObject *obj, const char *name, UValue value)
{
    USymbol *sym = (USymbol *)ustr_intern(vm, name, strlen(name));
    if (sym == NULL) return -1;
    return urbi_object_set_local_slot(vm, obj, sym, value);
}

/* Helper: turn a plain UObject pointer into a UVAL_OBJECT UValue. */
static UValue
uval_obj(UObject *obj)
{
    UValue v = urbi_make_nil();
    v.kind  = (uint8_t)UVAL_OBJECT;
    v.v.p   = (void *)obj;
    return v;
}

/* === Sub-test 1: existing slot on a UObject → URBI_OK + correct value === */

UTEST(slot_get_existing_local_slot)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *obj = make_object(&vm);
    UASSERT(obj != NULL);

    UValue stored = urbi_make_int(42);
    int rc = set_slot(&vm, obj, "x", stored);
    UASSERT_EQ(rc, 0);

    UValue out = urbi_make_nil();
    rc = urbi_slot_get(&vm, uval_obj(obj), "x", 1, &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT_EQ(out.v.i, 42);

    urbi_vm_destroy(&vm);
}

/* === Sub-test 2: slot through proto chain === */

UTEST(slot_get_via_proto_chain)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* parent holds slot "y" */
    UObject *parent = make_object(&vm);
    UASSERT(parent != NULL);
    UValue marker = urbi_make_int(99);
    int rc = set_slot(&vm, parent, "y", marker);
    UASSERT_EQ(rc, 0);

    /* child inherits from parent */
    UObject *child = make_object(&vm);
    UASSERT(child != NULL);
    rc = urbi_object_add_proto(&vm, child, parent);
    UASSERT_EQ(rc, 0);

    /* Query slot "y" on child — should walk up to parent. */
    UValue out = urbi_make_nil();
    rc = urbi_slot_get(&vm, uval_obj(child), "y", 1, &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT_EQ(out.v.i, 99);

    urbi_vm_destroy(&vm);
}

/* === Sub-test 3: missing slot → URBI_ERR_SLOT_NOT_FOUND === */

UTEST(slot_get_missing_returns_not_found)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *obj = make_object(&vm);
    UASSERT(obj != NULL);

    UValue out = urbi_make_nil();
    int rc = urbi_slot_get(&vm, uval_obj(obj), "no_such_slot", 12, &out);
    UASSERT_EQ(rc, URBI_ERR_SLOT_NOT_FOUND);

    urbi_vm_destroy(&vm);
}

/* === Sub-test 4: atom receiver (UVAL_INT) routes through Integer atom-proto === */

UTEST(slot_get_routes_atom_through_atom_proto)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Install a marker slot on the Integer atom-proto. */
    UObject *int_proto = urbi_object_atom(&vm, URBI_ATOM_INTEGER);
    UASSERT(int_proto != NULL);

    UValue marker = urbi_make_bool(true);
    USymbol *sym = (USymbol *)ustr_intern(&vm, "test_marker", 11);
    UASSERT(sym != NULL);
    int rc = urbi_object_set_local_slot(&vm, int_proto, sym, marker);
    UASSERT_EQ(rc, 0);

    /* Query via urbi_slot_get with an INT receiver. */
    UValue int_val = urbi_make_int(5);
    UValue out = urbi_make_nil();
    rc = urbi_slot_get(&vm, int_val, "test_marker", 11, &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_BOOL);
    UASSERT_EQ(out.v.i, 1);

    urbi_vm_destroy(&vm);
}

/* === Sub-test 5: NULL out_value → URBI_ERR_INVALID_ARG === */

UTEST(slot_get_null_out_returns_invalid_arg)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *obj = make_object(&vm);
    UASSERT(obj != NULL);

    int rc = urbi_slot_get(&vm, uval_obj(obj), "x", 1, NULL);
    UASSERT_EQ(rc, URBI_ERR_INVALID_ARG);

    urbi_vm_destroy(&vm);
}

/* === Suite entry === */

void
test_slot_get_suite(void)
{
    utest_run("slot_get_existing_local_slot",      slot_get_existing_local_slot);
    utest_run("slot_get_via_proto_chain",          slot_get_via_proto_chain);
    utest_run("slot_get_missing_returns_not_found", slot_get_missing_returns_not_found);
    utest_run("slot_get_routes_atom_through_atom_proto", slot_get_routes_atom_through_atom_proto);
    utest_run("slot_get_null_out_returns_invalid_arg", slot_get_null_out_returns_invalid_arg);
}
