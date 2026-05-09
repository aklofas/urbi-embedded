/* SPDX-License-Identifier: BSD-3-Clause */
/* test_atom_dispatch.c — Phase 2: atom-receiver to atom-proto routing.
 *
 * urbi_atom_proto_for_value(vm, v) returns the realm-global atom proto
 * for a primitive value (UVAL_INT → Integer; UVAL_FLOAT → Float; etc.).
 * For UVAL_OBJECT, returns the receiver itself (no atom routing).
 *
 * T19 unit cases (this file) — helper-only, no slow-path wiring yet.
 * T20 / T21 add end-to-end dispatch cases via compile + run.
 * T23 adds slot-set / shape-sentinel cases for the cleanup absorption. */

#include "utest.h"

#include "object/uobject.h"
#include "module/umodule.h"   /* UValue */
#include "vm/uvm.h"
#include "urbi/urbi.h"
#include "urbi/object.h"

#define UTEST(name) static void name(void)

/* === T19: atom-proto routing helper === */

UTEST(atom_proto_for_int) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue v = urbi_value_nil();
    v.kind = UVAL_INT;
    v.v.i = 42;

    UObject *proto = urbi_atom_proto_for_value(&vm, v);
    UASSERT(proto != NULL);
    UASSERT(proto == urbi_object_atom(&vm, URBI_ATOM_INTEGER));

    urbi_vm_destroy(&vm);
}

UTEST(atom_proto_for_float) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue v = urbi_value_nil();
    v.kind = UVAL_FLOAT;
    v.v.f = 3.14;

    UObject *proto = urbi_atom_proto_for_value(&vm, v);
    UASSERT(proto != NULL);
    UASSERT(proto == urbi_object_atom(&vm, URBI_ATOM_FLOAT));

    urbi_vm_destroy(&vm);
}

UTEST(atom_proto_for_string) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue v = urbi_value_nil();
    v.kind = UVAL_STR;
    v.v.p = NULL;  /* helper only inspects kind */

    UObject *proto = urbi_atom_proto_for_value(&vm, v);
    UASSERT(proto != NULL);
    UASSERT(proto == urbi_object_atom(&vm, URBI_ATOM_STRING));

    urbi_vm_destroy(&vm);
}

UTEST(atom_proto_for_object_returns_self) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *obj = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(obj != NULL);

    UValue v = urbi_value_nil();
    v.kind = UVAL_OBJECT;
    v.v.p = obj;

    UObject *proto = urbi_atom_proto_for_value(&vm, v);
    UASSERT_EQ((void *)proto, (void *)obj);

    urbi_vm_destroy(&vm);
}

void test_atom_dispatch_suite(void) {
    utest_run("atom_proto_for_int",                 atom_proto_for_int);
    utest_run("atom_proto_for_float",               atom_proto_for_float);
    utest_run("atom_proto_for_string",              atom_proto_for_string);
    utest_run("atom_proto_for_object_returns_self", atom_proto_for_object_returns_self);
}
