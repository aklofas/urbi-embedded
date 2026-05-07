/* SPDX-License-Identifier: BSD-3-Clause */
/* T74: const-slot diagnostic on global write.
 *
 * Verifies that:
 *  1. Writing to a constant built-in global slot (e.g. "Object") fails with
 *     the slot name included in vm->last_errmsg.
 *  2. Declaring a new non-const global slot succeeds.
 *  3. Reading an absent global slot fails with the slot name in vm->last_errmsg. */

#include "utest.h"

#include <stddef.h>
#include <string.h>

#include "value/uarena.h"
#include "uast.h"
#include "uemit.h"
#include "ulex.h"
#include "umodule.h"
#include "uparse.h"
#include "uvm.h"
#include "urbi/urbi.h"   /* urbi_run_chunk */

#define UTEST(name) static void name(void)

/* Helper: compile src + run under the VM's global realm.
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
    UValue out = {0};
    int rc = urbi_run_chunk(vm, NULL, &module, &out);
    uarena_destroy(&arena);
    umodule_destroy(&module);
    return rc;
}

/* === Tests === */

UTEST(top_level_var_object_raises_const_slot_write) {
    /* "var Object = 42" at chunk-top tries to overwrite the built-in
     * constant "Object" slot on the global object.  The IC slow path
     * detects URBI_SLOT_FLAG_CONSTANT and the VM halts with an error
     * message that includes the slot name. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    int rc = compile_and_run(&vm, "var Object = 42");

    /* Must fail at runtime (constant write rejected). */
    UASSERT(rc != URBI_OK);
    /* Error message must contain the slot name "Object". */
    UASSERT(strstr(vm.last_errmsg, "Object") != NULL);

    uvm_destroy(&vm);
}

UTEST(slot_assign_on_object_proto_works) {
    /* "var x = 42" at chunk-top installs a fresh (non-const) global slot
     * "x" on the global object.  There is no existing slot named "x", so
     * the IC slow path takes the miss path and installs it — no error. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    int rc = compile_and_run(&vm, "var x = 42");

    /* Must succeed. */
    UASSERT_EQ(URBI_OK, rc);

    uvm_destroy(&vm);
}

UTEST(unresolved_identifier_raises_slot_not_found) {
    /* Bare identifier "nonexistent_global_xyz" at chunk-top compiles to
     * a GETSLOT on the global object.  The slot doesn't exist; the IC
     * slow path returns a miss and the VM halts with an error message that
     * includes the slot name. */
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    int rc = compile_and_run(&vm, "nonexistent_global_xyz");

    /* Must fail at runtime (slot not found). */
    UASSERT(rc != URBI_OK);
    /* Error message must contain the missing slot name. */
    UASSERT(strstr(vm.last_errmsg, "nonexistent_global_xyz") != NULL);

    uvm_destroy(&vm);
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
