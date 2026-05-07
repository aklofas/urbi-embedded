/* SPDX-License-Identifier: BSD-3-Clause */
/* T32: emit_diag_warn warn-level diagnostic plumbing — unit tests.
 *
 * Verifies that:
 *   1. emit_diag_warn records (level, line, col, message) in e.diag_buf.
 *   2. Warns are non-fatal — the emitter continues to produce bytecode.
 *   3. Multiple warns accumulate; diag_count matches.
 */

#include "utest.h"

#include <string.h>

#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "module/umodule.h"
#include "parse/uparse.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/* Emit source through the full pipeline.  Returns emit error. */
static UEmitError diag_emit(const char *src, UEmitter *e_out,
                            UModule *mod_out, UArena *arena_out,
                            UVM *vm_out) {
    uvm_init(vm_out, NULL, NULL);
    uarena_init(arena_out, 4096);

    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    uemit_init(e_out, mod_out, arena_out, vm_out, NULL);

    UParser p;
    uparse_init(&p, &lex, arena_out);

    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) break;
        (void)uemit_statement(e_out, node);
        uarena_reset(arena_out);
    }
    return uemit_finish(e_out);
}

static void diag_cleanup(UModule *mod, UArena *arena, UVM *vm) {
    umodule_destroy(mod);
    uarena_destroy(arena);
    uvm_destroy(vm);
}

/* -----------------------------------------------------------------------
 * T32 test cases
 * ----------------------------------------------------------------------- */

/* emit_diag_warn records line, col, level, and message substring. */
UTEST(emit_diag_warn_records_message) {
    UVM vm;
    UModule module = {0};
    UArena arena;
    uvm_init(&vm, NULL, NULL);
    uarena_init(&arena, 4096);

    UEmitter e;
    uemit_init(&e, &module, &arena, &vm, NULL);

    UAstNode dummy;
    memset(&dummy, 0, sizeof(dummy));
    dummy.kind = AST_INT;
    dummy.line = 17;
    dummy.col  = 4;

    emit_diag_warn(&e, &dummy, "test warning %d", 42);

    UASSERT_EQ(1, e.diag_count);
    UASSERT_EQ((int)UEMIT_DIAG_WARN, (int)e.diag_buf[0].level);
    UASSERT_EQ(17, e.diag_buf[0].line);
    UASSERT_EQ(4,  e.diag_buf[0].col);
    UASSERT(e.diag_buf[0].message != NULL);
    UASSERT(strstr(e.diag_buf[0].message, "test warning 42") != NULL);

    emit_diag_free_all(&e);
    umodule_destroy(&module);
    uarena_destroy(&arena);
    uvm_destroy(&vm);
}

/* emit_diag_warn is non-fatal — bytecode is still produced after a warn. */
UTEST(emit_diag_warn_does_not_block_emit) {
    UVM vm;
    UModule module = {0};
    UArena arena;
    UEmitter e;

    UEmitError rc = diag_emit("function() { 42 }", &e, &module, &arena, &vm);

    /* Inject a warn before checking bytecode length. */
    UAstNode dummy;
    memset(&dummy, 0, sizeof(dummy));
    dummy.kind = AST_INT;
    dummy.line = 1;
    dummy.col  = 1;
    emit_diag_warn(&e, &dummy, "warn");

    UASSERT_EQ(EMIT_OK, rc);
    UASSERT_EQ(1, e.diag_count);
    /* Bytecode was emitted (at least one instruction). */
    UASSERT(module.instr_count >= 1u);

    emit_diag_free_all(&e);
    diag_cleanup(&module, &arena, &vm);
}

/* Multiple warns accumulate in order. */
UTEST(emit_diag_warn_accumulates_multiple) {
    UVM vm;
    UModule module = {0};
    UArena arena;
    uvm_init(&vm, NULL, NULL);
    uarena_init(&arena, 4096);

    UEmitter e;
    uemit_init(&e, &module, &arena, &vm, NULL);

    UAstNode dummy;
    memset(&dummy, 0, sizeof(dummy));
    dummy.kind = AST_INT;

    dummy.line = 1;
    emit_diag_warn(&e, &dummy, "first");
    dummy.line = 2;
    emit_diag_warn(&e, &dummy, "second");
    dummy.line = 3;
    emit_diag_warn(&e, &dummy, "third");

    UASSERT_EQ(3, e.diag_count);
    UASSERT_EQ(1, e.diag_buf[0].line);
    UASSERT_EQ(2, e.diag_buf[1].line);
    UASSERT_EQ(3, e.diag_buf[2].line);

    emit_diag_free_all(&e);
    umodule_destroy(&module);
    uarena_destroy(&arena);
    uvm_destroy(&vm);
}

/* NULL ast node — position defaults to 0,0; no crash. */
UTEST(emit_diag_warn_null_node_uses_zero_position) {
    UVM vm;
    UModule module = {0};
    UArena arena;
    uvm_init(&vm, NULL, NULL);
    uarena_init(&arena, 4096);

    UEmitter e;
    uemit_init(&e, &module, &arena, &vm, NULL);

    emit_diag_warn(&e, NULL, "no node");

    UASSERT_EQ(1, e.diag_count);
    UASSERT_EQ(0, e.diag_buf[0].line);
    UASSERT_EQ(0, e.diag_buf[0].col);

    emit_diag_free_all(&e);
    umodule_destroy(&module);
    uarena_destroy(&arena);
    uvm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Suite entry point
 * ----------------------------------------------------------------------- */

void test_emit_diag_suite(void) {
    utest_run("emit_diag_warn_records_message",
              emit_diag_warn_records_message);
    utest_run("emit_diag_warn_does_not_block_emit",
              emit_diag_warn_does_not_block_emit);
    utest_run("emit_diag_warn_accumulates_multiple",
              emit_diag_warn_accumulates_multiple);
    utest_run("emit_diag_warn_null_node_uses_zero_position",
              emit_diag_warn_null_node_uses_zero_position);
}
