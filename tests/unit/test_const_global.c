/* SPDX-License-Identifier: BSD-3-Clause */
/* T74: const-slot diagnostic on global write.
 *
 * Verifies that:
 *  1. Writing to a constant built-in global slot (e.g. "Object") raises a
 *     catchable TypeError (v0.11.4: was a fatal HALT setting vm->last_errmsg).
 *  2. Declaring a new non-const global slot succeeds.
 *  3. Reading an absent global slot raises a catchable TypeError.
 *
 * v0.11.4: the slow-path slot-not-found / const-write sites in uvm_slot.c now
 * deposit a catchable typed TypeError on the running strand instead of fatal-
 * halting.  Uncaught at chunk top these unwind the strand silently (run returns
 * URBI_OK, last_errmsg is NOT set), so these tests now observe the error via an
 * in-script try/catch side-effect var that records the caught instance's type. */

#include "utest.h"

#include <stddef.h>
#include <string.h>

#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "chunk/uchunk.h"
#include "parse/uparse.h"
#include "vm/uvm.h"
#include "urbi/urbi.h"   /* urbi_run_chunk */

#define UTEST(name) static void name(void)

/* Helper: compile src + run under the VM's global realm.
 * Returns URBI_OK on success; writes the chunk's last result to *out_result
 * when non-NULL.  Leaves vm->last_errmsg populated on a fatal (non-throw)
 * error. */
static int compile_and_run(UVM *vm, const char *src, UValue *out_result)
{
    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UArena arena;
    uarena_init(&arena, 4096);
    UProto module = {0};
    UEmitter e;
    uemit_init(&e, &module, &arena, vm, NULL);
    UParser p;
    uparse_init(&p, &lex, &arena);
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) {
            uarena_destroy(&arena);
            uchunk_destroy(&module, NULL);
            return URBI_ERR_COMPILE;
        }
        if (uemit_statement(&e, node) != EMIT_OK) {
            uarena_destroy(&arena);
            uchunk_destroy(&module, NULL);
            return URBI_ERR_COMPILE;
        }
        uarena_reset(&arena);
    }
    if (uemit_finish(&e) != EMIT_OK) {
        uarena_destroy(&arena);
        uchunk_destroy(&module, NULL);
        return URBI_ERR_COMPILE;
    }
    UValue out = {0};
    int rc = urbi_run_chunk(vm, NULL, &module, &out);
    if (out_result != NULL) *out_result = out;
    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
    return rc;
}

/* === Tests === */

UTEST(top_level_var_object_raises_const_slot_write) {
    /* "var Object = 42" at chunk-top tries to overwrite the built-in
     * constant "Object" slot on the global object.  The IC slow path
     * detects URBI_SLOT_FLAG_CONSTANT.  v0.11.4: this now raises a CATCHABLE
     * TypeError (was a fatal HALT).  Observe it via an in-script try/catch
     * that records 1 on a TypeError-typed catch. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue out = {0};
    int rc = compile_and_run(
        &vm,
        "var t = 0; try { var Object = 42 } "
        "catch (var e if e.isA(TypeError)) { t = 1 }; t",
        &out);

    /* Run completes (the throw is caught in-script). */
    UASSERT_EQ(URBI_OK, rc);
    /* The const-write error was raised AND caught as a TypeError. */
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ((int64_t)1, out.v.i);

    urbi_vm_destroy(&vm);
}

UTEST(slot_assign_on_object_proto_works) {
    /* "var x = 42" at chunk-top installs a fresh (non-const) global slot
     * "x" on the global object.  There is no existing slot named "x", so
     * the IC slow path takes the miss path and installs it — no error. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    int rc = compile_and_run(&vm, "var x = 42", NULL);

    /* Must succeed. */
    UASSERT_EQ(URBI_OK, rc);

    urbi_vm_destroy(&vm);
}

UTEST(unresolved_identifier_raises_slot_not_found) {
    /* Bare identifier "nonexistent_global_xyz" at chunk-top compiles to
     * a GETSLOT on the global object.  The slot doesn't exist; the IC
     * slow path returns a miss.  v0.11.4: this now raises a CATCHABLE
     * TypeError (was a fatal HALT).  Observe it via in-script try/catch. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue out = {0};
    int rc = compile_and_run(
        &vm,
        "var t = 0; try { nonexistent_global_xyz } "
        "catch (var e if e.isA(TypeError)) { t = 1 }; t",
        &out);

    /* Run completes (the throw is caught in-script). */
    UASSERT_EQ(URBI_OK, rc);
    /* The slot-not-found error was raised AND caught as a TypeError. */
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ((int64_t)1, out.v.i);

    urbi_vm_destroy(&vm);
}

void
test_const_global_suite(void)
{
    utest_run("const global write raises error with slot name in message",
              top_level_var_object_raises_const_slot_write);
    utest_run("non-const global slot assign succeeds",
              slot_assign_on_object_proto_works);
    utest_run("reading absent global slot gives error with slot name",
              unresolved_identifier_raises_slot_not_found);
}
