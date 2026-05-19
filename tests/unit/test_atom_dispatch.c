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
#include "object/uic.h"        /* UIC, urbi_slot_set_slow — T23 cleanup absorption */
#include "object/ushape.h"     /* urbi_shape_find_slot, URBI_SHAPE_SLOT_INVALID */
#include "chunk/umodule.h"   /* UValue, UModule */
#include "value/uintern.h"    /* ustr_intern — T20/T21 set local slots on atom protos */
#include "value/uarena.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "emit/uemit.h"
#include "vm/uvm.h"
#include "urbi/urbi.h"
#include "urbi/object.h"

#include <string.h>

#define UTEST(name) static void name(void)

/* Helper: compile + run src under the VM's global realm.
 * Returns URBI_OK on success; leaves vm->last_errmsg populated on error. */
static int compile_and_run(UVM *vm, const char *src)
{
    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UArena arena;
    uarena_init(&arena, 4096);
    UModule module = {0};
    UEmitter e;
    uemit_init(&e, &module, &arena, vm, NULL);
    UParser p;
    uparse_init(&p, &lex, &arena);
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) {
            uarena_destroy(&arena);
            umodule_destroy(&module, NULL);
            return URBI_ERR_COMPILE;
        }
        if (uemit_statement(&e, node) != EMIT_OK) {
            uarena_destroy(&arena);
            umodule_destroy(&module, NULL);
            return URBI_ERR_COMPILE;
        }
        uarena_reset(&arena);
    }
    if (uemit_finish(&e) != EMIT_OK) {
        uarena_destroy(&arena);
        umodule_destroy(&module, NULL);
        return URBI_ERR_COMPILE;
    }
    UValue out = {0};
    int rc = urbi_run_chunk(vm, NULL, &module, &out);
    uarena_destroy(&arena);
    umodule_destroy(&module, NULL);
    return rc;
}

/* === T19: atom-proto routing helper === */

UTEST(atom_proto_for_int) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue v = urbi_make_nil();
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

    UValue v = urbi_make_nil();
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

    UValue v = urbi_make_nil();
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

    UValue v = urbi_make_nil();
    v.kind = UVAL_OBJECT;
    v.v.p = obj;

    UObject *proto = urbi_atom_proto_for_value(&vm, v);
    UASSERT_EQ((void *)proto, (void *)obj);

    urbi_vm_destroy(&vm);
}

/* === T20: OP_GETSLOT slow path routes through atom proto === */

UTEST(int_method_dispatch_via_atom_proto) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Install a `marker` slot on the Integer atom proto so we can verify
     * that 1.marker resolves through atom-method dispatch. */
    UObject *int_proto = urbi_object_atom(&vm, URBI_ATOM_INTEGER);
    UASSERT(int_proto != NULL);
    USymbol *sym = (USymbol *)ustr_intern(&vm, "marker", 6);
    UASSERT(sym != NULL);

    UValue marker = urbi_make_nil();
    marker.kind = UVAL_INT;
    marker.v.i = 42;

    int rc = urbi_object_set_local_slot(&vm, int_proto, sym, marker);
    UASSERT_EQ(rc, 0);

    /* var v = 1.marker  →  v should be 42. */
    rc = compile_and_run(&vm, "var v = 1.marker");
    UASSERT_EQ(rc, URBI_OK);

    UValue out = urbi_make_nil();
    int grc = urbi_realm_get_global(&vm, urbi_realm_global(&vm), "v", 1, &out);
    UASSERT_EQ(grc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT_EQ((int)out.v.i, 42);

    urbi_vm_destroy(&vm);
}

/* === T21: integer-atom-dispatch returns the slot value (Phase 3 lands the
 *         no-alloc clone short-circuit; perf contract documented in
 *         src/object/uobject_atom_dispatch.c). === */

UTEST(int_method_dispatch_returns_slot_value) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Verify that dispatch from a UVAL_INT receiver carries the slot
     * value (not the receiver itself).  The Phase-3 carry-forward for
     * `clone` returning the receiver lands on the OP_CALL slow path,
     * not here.  Phase 2 only proves the dispatch arrives. */
    UObject *int_proto = urbi_object_atom(&vm, URBI_ATOM_INTEGER);
    UASSERT(int_proto != NULL);
    USymbol *sym = (USymbol *)ustr_intern(&vm, "kind_check", 10);
    UASSERT(sym != NULL);

    UValue marker = urbi_make_nil();
    marker.kind = UVAL_BOOL;
    marker.v.i = 1;

    int rc = urbi_object_set_local_slot(&vm, int_proto, sym, marker);
    UASSERT_EQ(rc, 0);

    rc = compile_and_run(&vm, "var v = 5.kind_check");
    UASSERT_EQ(rc, URBI_OK);

    UValue out = urbi_make_nil();
    int grc = urbi_realm_get_global(&vm, urbi_realm_global(&vm), "v", 1, &out);
    UASSERT_EQ(grc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_BOOL);
    UASSERT_EQ((int)out.v.i, 1);

    urbi_vm_destroy(&vm);
}

/* === T23: distinguish OOM from const-write in slot install/set ===
 *
 * Closes OBJ-007, OBJ-009, OBJ-017, API-007. */

UTEST(slot_set_slow_returns_const_slot_write_on_const_overwrite) {
    /* OBJ-007 + OBJ-009: urbi_slot_set_slow on a CONSTANT slot must
     * return URBI_ERR_CONST_SLOT_WRITE, not generic -1.  Pre-fix,
     * the const-write path collapsed onto the same -1 as OOM. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *obj = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(obj != NULL);
    USymbol *sym = (USymbol *)ustr_intern(&vm, "x", 1);
    UASSERT(sym != NULL);

    UValue v = urbi_make_nil();
    v.kind = UVAL_INT;
    v.v.i = 1;

    /* Install x as a regular slot, then mark it CONSTANT via property API. */
    int rc = urbi_object_set_local_slot(&vm, obj, sym, v);
    UASSERT_EQ(rc, 0);
    rc = urbi_object_install_property(&vm, obj, sym,
                                      URBI_SLOT_FLAG_CONSTANT, v);
    UASSERT_EQ(rc, 0);

    /* Attempt to overwrite via the slow-path SET (analogous to what
     * OP_SETSLOT does on const-write).  Pre-fix returned -1 (ambiguous);
     * post-fix returns URBI_ERR_CONST_SLOT_WRITE. */
    UIC ic;
    memset(&ic, 0, sizeof(ic));
    ic.name = sym;

    UValue v2 = urbi_make_nil();
    v2.kind = UVAL_INT;
    v2.v.i = 99;

    rc = urbi_slot_set_slow(&vm, obj, &ic, v2);
    UASSERT_EQ(rc, URBI_ERR_CONST_SLOT_WRITE);

    urbi_vm_destroy(&vm);
}

UTEST(shape_find_slot_uses_invalid_sentinel_on_miss) {
    /* OBJ-017: urbi_shape_find_slot must return URBI_SHAPE_SLOT_INVALID
     * (-1) instead of magic-number 0 to signal "slot not present".
     * Slot index 0 is a valid slot, never a miss sentinel. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *obj = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(obj != NULL);
    USymbol *sym_missing = (USymbol *)ustr_intern(&vm, "nonexistent", 11);
    UASSERT(sym_missing != NULL);

    int32_t idx = urbi_shape_find_slot(obj->shape, sym_missing);
    UASSERT_EQ(idx, URBI_SHAPE_SLOT_INVALID);

    urbi_vm_destroy(&vm);
}

void test_atom_dispatch_suite(void) {
    utest_run("atom_proto_for_int",                                       atom_proto_for_int);
    utest_run("atom_proto_for_float",                                     atom_proto_for_float);
    utest_run("atom_proto_for_string",                                    atom_proto_for_string);
    utest_run("atom_proto_for_object_returns_self",                       atom_proto_for_object_returns_self);
    utest_run("int_method_dispatch_via_atom_proto",                       int_method_dispatch_via_atom_proto);
    utest_run("int_method_dispatch_returns_slot_value",                   int_method_dispatch_returns_slot_value);
    utest_run("slot_set_slow_returns_const_slot_write_on_const_overwrite", slot_set_slow_returns_const_slot_write_on_const_overwrite);
    utest_run("shape_find_slot_uses_invalid_sentinel_on_miss",            shape_find_slot_uses_invalid_sentinel_on_miss);
}
