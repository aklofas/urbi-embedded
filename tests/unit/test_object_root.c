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
#include "object/uic.h"
#include "chunk/uchunk.h"
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

/* === T30: UClosure.native_fn dispatch via OP_CALL =========================== */

static int
test_native_returns_42(UVM *vm, UValue self, UValue *args, uint8_t nargs,
                       UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    *out = urbi_make_nil();
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
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_CLOSURE;
    v.v.p = cl;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, root, sym, v), 0);

    /* var v = Object.fortyTwo()  →  42 */
    UASSERT_EQ(compile_and_run(&vm, "var v = Object.fortyTwo()"), URBI_OK);

    UValue out = urbi_make_nil();
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

    UValue out = urbi_make_nil();
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

    UValue va = urbi_make_nil();
    UValue vb = urbi_make_nil();
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

    UValue vb = urbi_make_nil();
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

    UValue out = urbi_make_nil();
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

    UValue vb = urbi_make_nil();
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

    UValue out = urbi_make_nil();
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

    UValue ov = urbi_make_nil();
    UValue cv = urbi_make_nil();
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
    /* The compile-and-run sequence allocates a UModule + UChunkInstance
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

    UValue out = urbi_make_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "v", 1, &out), URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT_EQ((int)out.v.i, 42);

    urbi_vm_destroy(&vm);
}

/* === T57: clone can overwrite a const-inherited slot =====================
 *
 * Touchstone for legacy/repos/aldebaran-urbi/tests/2.x/slot-cow-const.chk:
 *
 *   class a { const var x = 0 } |;
 *   var b = a.new() |;
 *   b.x = 12;     // [00000001] 12   — accepted
 *   a.x;          // [00000002] 0    — unchanged
 *
 * The semantic: a's `x` is const; b inherits via clone; b can override its
 * own slot for x, leaving a's x untouched.  The COW write SUCCEEDS for the
 * derived even though the source slot is constant — const-ness is an
 * attribute of the source slot, not a constraint on derivations.
 *
 * Pre-T57 baseline urbi_slot_set_slow rejected the write with
 * URBI_ERR_CONST_SLOT_WRITE because the const-flag check came before the
 * holder == recv discrimination.  T57 narrows the const check to the
 * holder == recv case so COW writes proceed for derived objects. */

UTEST(clone_can_overwrite_const_inherited_slot) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)urbi_realm_global(&vm);

    UObject *p = urbi_object_alloc(&vm, URBI_ATOM_OBJECT);
    UASSERT(p != NULL);

    USymbol *sym_x = (USymbol *)ustr_intern(&vm, "x", 1);
    UASSERT(sym_x != NULL);

    UValue zero = urbi_make_nil();
    zero.kind = (uint8_t)UVAL_INT;
    zero.v.i = 0;
    UASSERT_EQ(urbi_object_set_local_slot(&vm, p, sym_x, zero), 0);
    UASSERT_EQ(urbi_object_install_property(&vm, p, sym_x,
                                            URBI_SLOT_FLAG_CONSTANT, zero),
               URBI_OK);

    /* Clone: b.x should COW-clone the const slot but the local copy must
     * be mutable. */
    UObject *b = urbi_object_clone(&vm, p);
    UASSERT(b != NULL);

    UIC ic;
    memset(&ic, 0, sizeof(ic));
    ic.name = sym_x;

    UValue twelve = urbi_make_nil();
    twelve.kind = (uint8_t)UVAL_INT;
    twelve.v.i = 12;
    UASSERT_EQ(urbi_slot_set_slow(&vm, b, &ic, twelve), URBI_OK);

    /* Reset the IC for the read-back; the prior set_slow may have left a
     * cached entry but that doesn't matter here since urbi_slot_get_slow
     * also writes into the cache from scratch (by re-resolving on miss). */
    memset(&ic, 0, sizeof(ic));
    ic.name = sym_x;

    UValue pv = urbi_make_nil();
    UASSERT_EQ(urbi_slot_get_slow(&vm, p, &ic, &pv), 0);
    UASSERT_EQ((int)pv.v.i, 0);

    memset(&ic, 0, sizeof(ic));
    ic.name = sym_x;
    UValue bv = urbi_make_nil();
    UASSERT_EQ(urbi_slot_get_slow(&vm, b, &ic, &bv), 0);
    UASSERT_EQ((int)bv.v.i, 12);

    urbi_vm_destroy(&vm);
}

/* === T56: COW semantics through .new() ===================================
 *
 * The M4 COW machinery handles writes-on-clones: c.setSlot writes a fresh
 * local slot on c without disturbing p's slot.  Phase-5 .new() delegates
 * to .clone() so the COW path activates transparently. */

UTEST(new_then_local_write_does_not_modify_proto) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)urbi_realm_global(&vm);

    const char *src =
        "var p = Object.clone();"
        "p.setSlot(\"x\", 42);"
        "var c = p.new();"
        "c.setSlot(\"x\", 99);"
        "var pv = p.x;"
        "var cv = c.x";
    UASSERT_EQ(compile_and_run(&vm, src), URBI_OK);

    UValue pv = urbi_make_nil();
    UValue cv = urbi_make_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "pv", 2, &pv), URBI_OK);
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "cv", 2, &cv), URBI_OK);
    UASSERT_EQ((int)pv.v.i, 42);
    UASSERT_EQ((int)cv.v.i, 99);

    urbi_vm_destroy(&vm);
}

/* === T63: protos.insertFront(proto) ========================================
 *
 * The legacy shared-protos.chk fixture (line 12) uses
 * `C.protos.insertFront(A)` to prepend A onto C's prototype list.  The
 * Wave-1 stub installs insertFront on the synthetic proto-list returned
 * by .protos and threads the owner through a hidden _owner slot. */

UTEST(protos_insert_front_prepends_proto) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)urbi_realm_global(&vm);

    /* a has slot foo=1; b has slot foo=2.  c clones b (so c.foo == 2);
     * c.protos().insertFront(a) prepends a, so c.foo now resolves to a's
     * slot first (== 1).  Wave-1: protos is a method (parens required);
     * Wave-2's List atom may make protos a property (parens optional). */
    const char *src =
        "var a = Object.clone();"
        "a.setSlot(\"foo\", 1);"
        "var b = Object.clone();"
        "b.setSlot(\"foo\", 2);"
        "var c = b.clone();"
        "var v0 = c.foo;"
        "c.protos().insertFront(a);"
        "var v1 = c.foo";
    UASSERT_EQ(compile_and_run(&vm, src), URBI_OK);

    UValue v0 = urbi_make_nil();
    UValue v1 = urbi_make_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "v0", 2, &v0), URBI_OK);
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "v1", 2, &v1), URBI_OK);
    UASSERT_EQ((int)v0.v.i, 2);
    UASSERT_EQ((int)v1.v.i, 1);

    urbi_vm_destroy(&vm);
}

/* === T62: removeLocalSlot legacy alias =====================================
 *
 * Object.removeLocalSlot is a legacy alias for removeSlot used in the
 * 2014 inheritance.chk fixture (line 36).  Same semantics — drop the
 * slot from the receiver's own shape; no proto-chain walk. */

UTEST(object_remove_local_slot_alias) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)urbi_realm_global(&vm);

    const char *src =
        "var o = Object.clone();"
        "o.setSlot(\"x\", 42);"
        "o.removeLocalSlot(\"x\");"
        "var b = o.hasSlot(\"x\")";
    UASSERT_EQ(compile_and_run(&vm, src), URBI_OK);

    UValue out = urbi_make_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "b", 1, &out), URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_BOOL);
    UASSERT_EQ((int)out.v.i, 0);

    urbi_vm_destroy(&vm);
}

/* === T61: getSlotValue legacy alias ========================================
 *
 * Object.getSlotValue is a legacy alias for getSlot used in the 2014
 * inheritance.chk fixture (line 17).  Same semantics — walk the proto
 * chain and return the resolved value. */

UTEST(object_get_slot_value_alias) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)urbi_realm_global(&vm);

    const char *src =
        "var p = Object.clone();"
        "p.setSlot(\"foo\", 100);"
        "var c = p.new();"
        "var v = c.getSlotValue(\"foo\")";
    UASSERT_EQ(compile_and_run(&vm, src), URBI_OK);

    UValue out = urbi_make_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "v", 1, &out), URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT_EQ((int)out.v.i, 100);

    urbi_vm_destroy(&vm);
}

/* === T60: three-level clone chain lookup =================================
 *
 * a.new() → b; b.new() → c.  c.x must resolve through the chain back to
 * a's slot via prototype-graph DFS.  Confirms the urbi_object_resolve_slot
 * walker correctly traverses two hops without dropping the trail. */

UTEST(clone_chain_three_levels) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)urbi_realm_global(&vm);

    const char *src =
        "var a = Object.clone();"
        "a.setSlot(\"x\", 42);"
        "var b = a.new();"
        "var c = b.new();"
        "var v = c.x";
    UASSERT_EQ(compile_and_run(&vm, src), URBI_OK);

    UValue out = urbi_make_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "v", 1, &out), URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT_EQ((int)out.v.i, 42);

    urbi_vm_destroy(&vm);
}

/* === T59: atom .new() returns self (S-atom-clone-perf) ===================
 *
 * For non-UVAL_OBJECT receivers, .new() short-circuits via obj_clone's
 * atom path and returns the receiver itself with zero allocation.  This
 * mirrors the .clone() contract; the legacy fixture
 * legacy/repos/aldebaran-urbi/tests/2.x/atom-clone.chk drove the
 * underlying urbi_object_clone semantics. */

UTEST(atom_new_returns_self) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    (void)urbi_realm_global(&vm);

    UASSERT_EQ(compile_and_run(&vm, "var v = 7.new()"), URBI_OK);

    UValue out = urbi_make_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "v", 1, &out), URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT_EQ((int)out.v.i, 7);

    /* String literal: identity-preserving (the interned USymbol pointer
     * is the same as the source 1-shot literal). */
    UASSERT_EQ(compile_and_run(&vm, "var s = \"foo\".new()"), URBI_OK);
    UValue sout = urbi_make_nil();
    UASSERT_EQ(urbi_realm_get_global(&vm, urbi_realm_global(&vm), "s", 1, &sout), URBI_OK);
    UASSERT_EQ((int)sout.kind, (int)UVAL_STR);

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

    UValue av = urbi_make_nil();
    UValue bv = urbi_make_nil();
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

    UValue out = urbi_make_nil();
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
    utest_run("object_root: clone can overwrite const-inherited slot",
              clone_can_overwrite_const_inherited_slot);
    utest_run("object_root: .new() then local write does not modify proto",
              new_then_local_write_does_not_modify_proto);
    utest_run("object_root: atom .new() returns self",
              atom_new_returns_self);
    utest_run("object_root: clone-chain three-level lookup",
              clone_chain_three_levels);
    utest_run("object_root: getSlotValue is a legacy alias for getSlot",
              object_get_slot_value_alias);
    utest_run("object_root: removeLocalSlot is a legacy alias for removeSlot",
              object_remove_local_slot_alias);
    utest_run("object_root: protos.insertFront prepends proto",
              protos_insert_front_prepends_proto);
    utest_run("object_root: .new() and .clone() are equivalent",
              new_and_clone_are_equivalent);
}
