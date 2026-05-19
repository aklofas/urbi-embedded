/* SPDX-License-Identifier: BSD-3-Clause */
/* test_atom_protos.c — M6 Phase 4: atom proto stubs.
 *
 * Each atom family has a dedicated realm-global proto with at least
 * .clone() and a minimum method set. Wave 2 will expand these. */

#include "utest.h"

#include "object/uobject.h"
#include "chunk/uchunk.h"
#include "value/uintern.h"
#include "value/uarena.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "emit/uemit.h"
#include "stdlib/atom_protos.h"
#include "stdlib/stdlib_boot.h"
#include "realm/urealm_globals.h"
#include "vm/uvm.h"
#include "urbi/urbi.h"
#include "urbi/object.h"
#include "urbi/types.h"

#include <string.h>

#define UTEST(name) static void name(void)

/* Helper: compile + run src under the VM's global realm. */
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
    UValue out = urbi_make_nil();
    int rc = urbi_run_chunk(vm, NULL, &module, &out);
    uarena_destroy(&arena);
    umodule_destroy(&module, NULL);
    return rc;
}

/* === T45: URBI_ATOM_BOOLEAN / NIL / VOID lazy singletons === */

UTEST(atom_boolean_proto_exists) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *bool_proto = urbi_object_atom(&vm, URBI_ATOM_BOOLEAN);
    UASSERT(bool_proto != NULL);
    /* Distinct from root Object. */
    UASSERT(bool_proto != urbi_object_root(&vm));
    /* Idempotent — second call returns the cached singleton. */
    UASSERT(bool_proto == urbi_object_atom(&vm, URBI_ATOM_BOOLEAN));

    urbi_vm_destroy(&vm);
}

UTEST(atom_nil_proto_exists) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *nil_proto = urbi_object_atom(&vm, URBI_ATOM_NIL);
    UASSERT(nil_proto != NULL);
    UASSERT(nil_proto != urbi_object_root(&vm));
    UASSERT(nil_proto != urbi_object_atom(&vm, URBI_ATOM_BOOLEAN));

    urbi_vm_destroy(&vm);
}

UTEST(atom_void_proto_exists) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UObject *void_proto = urbi_object_atom(&vm, URBI_ATOM_VOID);
    UASSERT(void_proto != NULL);
    UASSERT(void_proto != urbi_object_root(&vm));
    UASSERT(void_proto != urbi_object_atom(&vm, URBI_ATOM_NIL));

    urbi_vm_destroy(&vm);
}

/* === T46: urbi_atom_proto_for_value routes UVAL_BOOL / NIL / VOID === */

UTEST(atom_proto_for_bool_routes_to_boolean) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue v = urbi_make_nil();
    v.kind = UVAL_BOOL;
    v.v.i = 1;

    UObject *proto = urbi_atom_proto_for_value(&vm, v);
    UASSERT(proto == urbi_object_atom(&vm, URBI_ATOM_BOOLEAN));

    urbi_vm_destroy(&vm);
}

UTEST(atom_proto_for_nil_routes_to_nil) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue v = urbi_make_nil();   /* kind == UVAL_NIL */

    UObject *proto = urbi_atom_proto_for_value(&vm, v);
    UASSERT(proto == urbi_object_atom(&vm, URBI_ATOM_NIL));

    urbi_vm_destroy(&vm);
}

UTEST(atom_proto_for_void_routes_to_void) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue v = urbi_make_nil();
    v.kind = UVAL_VOID;

    UObject *proto = urbi_atom_proto_for_value(&vm, v);
    UASSERT(proto == urbi_object_atom(&vm, URBI_ATOM_VOID));

    urbi_vm_destroy(&vm);
}

/* === T47: atom-proto C-native methods (Boolean.toString, String.length) === */

UTEST(boolean_toString_true) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Method invocation requires parens — bare `true.toString` returns the
     * closure handle (UVAL_CLOSURE) without calling it. */
    int rc = compile_and_run(&vm, "var v = true.toString()");
    UASSERT_EQ(rc, URBI_OK);

    UValue out = urbi_make_nil();
    int grc = urbi_realm_get_global(&vm, urbi_realm_global(&vm), "v", 1, &out);
    UASSERT_EQ(grc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_STR);

    urbi_vm_destroy(&vm);
}

UTEST(string_length_returns_byte_count) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    int rc = compile_and_run(&vm, "var v = \"hello\".length()");
    UASSERT_EQ(rc, URBI_OK);

    UValue out = urbi_make_nil();
    int grc = urbi_realm_get_global(&vm, urbi_realm_global(&vm), "v", 1, &out);
    UASSERT_EQ(grc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT_EQ((int)out.v.i, 5);

    urbi_vm_destroy(&vm);
}

/* === T48: realm-global Boolean / Nil / Void resolve to atom protos === */

UTEST(realm_global_Boolean_is_atom_boolean) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue out = urbi_make_nil();
    int grc = urbi_realm_get_global(&vm, urbi_realm_global(&vm),
                                    "Boolean", 7, &out);
    UASSERT_EQ(grc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_OBJECT);
    UASSERT(out.v.p == urbi_object_atom(&vm, URBI_ATOM_BOOLEAN));

    urbi_vm_destroy(&vm);
}

UTEST(realm_global_Nil_is_atom_nil_proto) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue out = urbi_make_nil();
    int grc = urbi_realm_get_global(&vm, urbi_realm_global(&vm),
                                    "Nil", 3, &out);
    UASSERT_EQ(grc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_OBJECT);
    UASSERT(out.v.p == urbi_object_atom(&vm, URBI_ATOM_NIL));

    urbi_vm_destroy(&vm);
}

UTEST(realm_global_Void_is_atom_void_proto) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue out = urbi_make_nil();
    int grc = urbi_realm_get_global(&vm, urbi_realm_global(&vm),
                                    "Void", 4, &out);
    UASSERT_EQ(grc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_OBJECT);
    UASSERT(out.v.p == urbi_object_atom(&vm, URBI_ATOM_VOID));

    urbi_vm_destroy(&vm);
}

/* `nil` value singleton remains a UVAL_NIL (separate row from Nil proto). */
UTEST(realm_global_lower_nil_is_value_singleton) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue out = urbi_make_nil();
    out.kind = UVAL_INT;   /* poison so we can detect it gets overwritten */
    int grc = urbi_realm_get_global(&vm, urbi_realm_global(&vm),
                                    "nil", 3, &out);
    UASSERT_EQ(grc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_NIL);

    urbi_vm_destroy(&vm);
}

void
test_atom_protos_suite(void)
{
    utest_run("atom_boolean_proto_exists",
              atom_boolean_proto_exists);
    utest_run("atom_nil_proto_exists",
              atom_nil_proto_exists);
    utest_run("atom_void_proto_exists",
              atom_void_proto_exists);
    utest_run("atom_proto_for_bool_routes_to_boolean",
              atom_proto_for_bool_routes_to_boolean);
    utest_run("atom_proto_for_nil_routes_to_nil",
              atom_proto_for_nil_routes_to_nil);
    utest_run("atom_proto_for_void_routes_to_void",
              atom_proto_for_void_routes_to_void);
    utest_run("boolean_toString_true",
              boolean_toString_true);
    utest_run("string_length_returns_byte_count",
              string_length_returns_byte_count);
    utest_run("realm_global_Boolean_is_atom_boolean",
              realm_global_Boolean_is_atom_boolean);
    utest_run("realm_global_Nil_is_atom_nil_proto",
              realm_global_Nil_is_atom_nil_proto);
    utest_run("realm_global_Void_is_atom_void_proto",
              realm_global_Void_is_atom_void_proto);
    utest_run("realm_global_lower_nil_is_value_singleton",
              realm_global_lower_nil_is_value_singleton);
}
