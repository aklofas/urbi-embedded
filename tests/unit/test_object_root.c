/* SPDX-License-Identifier: BSD-3-Clause */
/* test_object_root.c — M6 Phase 3: Object root C-native methods.
 *
 * Covers T30-T37 of the v0.6.0-stdlib-scaffold plan:
 *   T30 — UClosure.native_fn dispatch via OP_CALL.
 *   T31 — Object.setSlot end-to-end.
 *   T33 — getSlot / hasSlot / removeSlot.
 *   T34 — addProto / removeProto.
 *   T35 — clone with atom short-circuit + UObject fresh-alloc.
 *   T36 — atom-clone zero-allocation contract (S-atom-clone-perf).
 *   T37 — setProtos single-UObject path (Wave-1 limited).
 *
 * Each test compiles a small urbiscript snippet, runs it, and reads back
 * a global to assert the expected value.  Tests rely on stdlib boot
 * being driven by urbi_realm_global → urbi_populate_realm_globals. */

#include "utest.h"

#include "object/uobject.h"
#include "module/umodule.h"
#include "value/uintern.h"
#include "value/uarena.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "emit/uemit.h"
#include "stdlib/object_root.h"
#include "stdlib/stdlib_boot.h"
#include "runtime/uclosure.h"
#include "vm/uvm.h"
#include "urbi/urbi.h"
#include "urbi/object.h"

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
            umodule_destroy(&module);
            return URBI_ERR_COMPILE;
        }
        if (uemit_statement(&e, node) != EMIT_OK) {
            uarena_destroy(&arena);
            umodule_destroy(&module);
            return URBI_ERR_COMPILE;
        }
        uarena_reset(&arena);
    }
    if (uemit_finish(&e) != EMIT_OK) {
        uarena_destroy(&arena);
        umodule_destroy(&module);
        return URBI_ERR_COMPILE;
    }
    UValue out = urbi_value_nil();
    int rc = urbi_run_chunk(vm, NULL, &module, &out);
    uarena_destroy(&arena);
    umodule_destroy(&module);
    return rc;
}

/* === T30: UClosure.native_fn dispatch via OP_CALL =========================== */

static int
test_native_returns_42(UVM *vm, UValue self, UValue *args, uint8_t nargs,
                       UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    *out = urbi_value_nil();
    out->kind = (uint8_t)UVAL_INT;
    out->v.i = 42;
    return UEXEC_OK;
}

UTEST(native_fn_dispatched_via_op_call) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Touch the global realm so stdlib boot runs (Object proto exists). */
    (void)urbi_realm_global(&vm);

    /* Install our test native on Object as `fortyTwo`. */
    UObject *root = urbi_object_root(&vm);
    UASSERT(root != NULL);
    UClosure *cl = urbi_native_closure_create(&vm, test_native_returns_42);
    UASSERT(cl != NULL);

    USymbol *sym = (USymbol *)ustr_intern(&vm, "fortyTwo", 8);
    UASSERT(sym != NULL);
    UValue v = urbi_value_nil();
    v.kind = (uint8_t)UVAL_CLOSURE;
    v.v.p = cl;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, root, sym, v), 0);

    /* var v = Object.fortyTwo()  →  42 */
    UASSERT_EQ(compile_and_run(&vm, "var v = Object.fortyTwo()"), URBI_OK);

    UValue out = urbi_value_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm),
                                     "v", 1, &out), URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT_EQ((int)out.v.i, 42);

    urbi_vm_destroy(&vm);
}

/* === T31: Object.setSlot end-to-end ======================================== */

UTEST(object_set_slot_then_read_back) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)urbi_realm_global(&vm);

    const char *src =
        "var o = Object.clone();"
        "o.setSlot(\"x\", 42);"
        "var v = o.getSlot(\"x\")";
    UASSERT_EQ(compile_and_run(&vm, src), URBI_OK);

    UValue out = urbi_value_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm),
                                     "v", 1, &out), URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT_EQ((int)out.v.i, 42);

    urbi_vm_destroy(&vm);
}

/* === T33: getSlot / hasSlot / removeSlot =================================== */

UTEST(object_has_slot_present_and_absent) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)urbi_realm_global(&vm);

    const char *src =
        "var o = Object.clone();"
        "o.setSlot(\"x\", 42);"
        "var a = o.hasSlot(\"x\");"
        "var b = o.hasSlot(\"y\")";
    UASSERT_EQ(compile_and_run(&vm, src), URBI_OK);

    UValue va = urbi_value_nil();
    UValue vb = urbi_value_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "a", 1, &va), URBI_OK);
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "b", 1, &vb), URBI_OK);
    UASSERT_EQ((int)va.kind, (int)UVAL_BOOL);
    UASSERT_EQ((int)va.v.i, 1);
    UASSERT_EQ((int)vb.kind, (int)UVAL_BOOL);
    UASSERT_EQ((int)vb.v.i, 0);

    urbi_vm_destroy(&vm);
}

UTEST(object_remove_slot_removes) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)urbi_realm_global(&vm);

    const char *src =
        "var o = Object.clone();"
        "o.setSlot(\"x\", 42);"
        "o.removeSlot(\"x\");"
        "var b = o.hasSlot(\"x\")";
    UASSERT_EQ(compile_and_run(&vm, src), URBI_OK);

    UValue vb = urbi_value_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "b", 1, &vb), URBI_OK);
    UASSERT_EQ((int)vb.kind, (int)UVAL_BOOL);
    UASSERT_EQ((int)vb.v.i, 0);

    urbi_vm_destroy(&vm);
}

/* === T34: addProto / removeProto =========================================== */

UTEST(object_add_proto_inherits_slot) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)urbi_realm_global(&vm);

    const char *src =
        "var p = Object.clone();"
        "p.setSlot(\"foo\", 100);"
        "var c = Object.clone();"
        "c.addProto(p);"
        "var v = c.foo";
    UASSERT_EQ(compile_and_run(&vm, src), URBI_OK);

    UValue out = urbi_value_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "v", 1, &out), URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT_EQ((int)out.v.i, 100);

    urbi_vm_destroy(&vm);
}

UTEST(object_remove_proto_drops_inheritance) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)urbi_realm_global(&vm);

    const char *src =
        "var p = Object.clone();"
        "p.setSlot(\"foo\", 100);"
        "var c = Object.clone();"
        "c.addProto(p);"
        "c.removeProto(p);"
        "var b = c.hasSlot(\"foo\")";
    UASSERT_EQ(compile_and_run(&vm, src), URBI_OK);

    UValue vb = urbi_value_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "b", 1, &vb), URBI_OK);
    UASSERT_EQ((int)vb.kind, (int)UVAL_BOOL);
    UASSERT_EQ((int)vb.v.i, 0);

    urbi_vm_destroy(&vm);
}

/* === T35: clone (atom short-circuit + UObject fresh-alloc) ================= */

UTEST(clone_int_returns_int) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)urbi_realm_global(&vm);

    UASSERT_EQ(compile_and_run(&vm, "var v = 1.clone()"), URBI_OK);

    UValue out = urbi_value_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "v", 1, &out), URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT_EQ((int)out.v.i, 1);

    urbi_vm_destroy(&vm);
}

UTEST(clone_object_allocates_fresh) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)urbi_realm_global(&vm);

    const char *src =
        "var o = Object.clone();"
        "o.setSlot(\"x\", 42);"
        "var c = o.clone();"
        "c.setSlot(\"x\", 99);"
        "var ov = o.x;"
        "var cv = c.x";
    UASSERT_EQ(compile_and_run(&vm, src), URBI_OK);

    UValue ov = urbi_value_nil();
    UValue cv = urbi_value_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "ov", 2, &ov), URBI_OK);
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "cv", 2, &cv), URBI_OK);
    UASSERT_EQ((int)ov.v.i, 42);
    UASSERT_EQ((int)cv.v.i, 99);

    urbi_vm_destroy(&vm);
}

/* === T36: atom-clone zero-allocation contract (S-atom-clone-perf) ==========
 *
 * Loop 1000.times-equivalent (while), confirm no per-iteration UObject
 * alloc.  We allow a small constant for IC + module setup; the loop body
 * itself must not allocate. */

UTEST(atom_clone_zero_allocations) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)urbi_realm_global(&vm);

    /* Warm up: ensure stdlib boot + globals install + first compile-run
     * happen BEFORE the measurement window.  Phase 3 baseline allocates
     * tens of KB during the very first urbi_realm_global call (this is
     * the boot allocation, not per-call). */
    UASSERT_EQ(compile_and_run(&vm, "var w = 1.clone()"), URBI_OK);

    size_t pre = vm.gc_total_allocated;

    /* Loop 1000 times calling i.clone() — each i.clone() must short-
     * circuit on the atom path. */
    const char *src =
        "var i = 0;"
        "while (i < 1000) { var x = i.clone(); i = i + 1 }";
    UASSERT_EQ(compile_and_run(&vm, src), URBI_OK);

    size_t post = vm.gc_total_allocated;
    /* The compile-and-run sequence allocates a UModule + UModuleInstance
     * + IC tables for the parse — that's the script overhead, not the
     * loop-body overhead.  But the LOOP itself (1000 iterations) must
     * allocate nothing.  We can't separate "compile cost" from "loop
     * cost" perfectly here, but the loop adds 56 B per iteration if
     * clone() allocates — i.e. ~56 KB.  A delta under ~10 KB confirms
     * the short-circuit is working (the bulk of overhead is one-time
     * setup, never per iteration). */
    UASSERT(post - pre < 10240U);

    urbi_vm_destroy(&vm);
}

/* === T55: Object.new returns a clone (Class.new() idiom) =================== */

UTEST(object_new_returns_clone) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)urbi_realm_global(&vm);

    const char *src =
        "var p = Object.clone();"
        "p.setSlot(\"x\", 42);"
        "var c = p.new();"
        "var v = c.x";
    UASSERT_EQ(compile_and_run(&vm, src), URBI_OK);

    UValue out = urbi_value_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "v", 1, &out), URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT_EQ((int)out.v.i, 42);

    urbi_vm_destroy(&vm);
}

UTEST(new_and_clone_are_equivalent) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)urbi_realm_global(&vm);

    const char *src =
        "var p = Object.clone();"
        "p.setSlot(\"x\", 100);"
        "var a = p.new();"
        "var b = p.clone();"
        "var av = a.x;"
        "var bv = b.x";
    UASSERT_EQ(compile_and_run(&vm, src), URBI_OK);

    UValue av = urbi_value_nil();
    UValue bv = urbi_value_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "av", 2, &av), URBI_OK);
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "bv", 2, &bv), URBI_OK);
    UASSERT_EQ((int)av.v.i, 100);
    UASSERT_EQ((int)bv.v.i, 100);

    urbi_vm_destroy(&vm);
}

/* === T37: setProtos single-UObject path (Wave-1 limited) =================== */

UTEST(object_set_protos_single) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)urbi_realm_global(&vm);

    const char *src =
        "var p = Object.clone();"
        "p.setSlot(\"foo\", 100);"
        "var c = Object.clone();"
        "c.setProtos(p);"
        "var v = c.foo";
    UASSERT_EQ(compile_and_run(&vm, src), URBI_OK);

    UValue out = urbi_value_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "v", 1, &out), URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT_EQ((int)out.v.i, 100);

    urbi_vm_destroy(&vm);
}

/* === Suite registration ==================================================== */

void test_object_root_suite(void) {
    utest_run("object_root: native_fn dispatched via OP_CALL",
              native_fn_dispatched_via_op_call);
    utest_run("object_root: setSlot then read back",
              object_set_slot_then_read_back);
    utest_run("object_root: hasSlot present and absent",
              object_has_slot_present_and_absent);
    utest_run("object_root: removeSlot removes",
              object_remove_slot_removes);
    utest_run("object_root: addProto inherits slot",
              object_add_proto_inherits_slot);
    utest_run("object_root: removeProto drops inheritance",
              object_remove_proto_drops_inheritance);
    utest_run("object_root: clone(int) returns int",
              clone_int_returns_int);
    utest_run("object_root: clone(obj) allocates fresh",
              clone_object_allocates_fresh);
    utest_run("object_root: atom-clone zero allocations",
              atom_clone_zero_allocations);
    utest_run("object_root: setProtos single UObject",
              object_set_protos_single);
    utest_run("object_root: Object.new returns a clone",
              object_new_returns_clone);
    utest_run("object_root: .new() and .clone() are equivalent",
              new_and_clone_are_equivalent);
}
