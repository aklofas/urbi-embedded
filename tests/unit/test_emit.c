/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include <stdlib.h>
#include <string.h>

#include "uarena.h"
#include "uemit.h"

#define UTEST(name) static void name(void)

UTEST(uemit_init_zeros_emitter_and_does_not_touch_chunk) {
    Chunk chunk = {0};
    Arena arena;
    uarena_init(&arena, 0);
    Emitter e;
    uemit_init(&e, &chunk, &arena, "repl");

    UASSERT_EQ((uint8_t)0, e.next_reg);
    UASSERT_EQ((uint8_t)0, e.max_reg_seen);
    UASSERT_EQ((uint32_t)0, e.prev_line);
    UASSERT(e.any_stmt_emitted == false);
    UASSERT(e.finished == false);
    UASSERT_EQ(EMIT_OK, e.error);
    UASSERT_EQ((Chunk *)&chunk, e.chunk);
    UASSERT_EQ((Arena *)&arena, e.arena);
    UASSERT_EQ((size_t)0, chunk.instr_count);
    uarena_destroy(&arena);
    uchunk_destroy(&chunk);
}

UTEST(uemit_finish_on_empty_chunk_emits_nothing_and_returns_ok) {
    Chunk chunk = {0};
    Arena arena;
    uarena_init(&arena, 0);
    Emitter e;
    uemit_init(&e, &chunk, &arena, NULL);
    EmitError rc = uemit_finish(&e);
    UASSERT_EQ(EMIT_OK, rc);
    UASSERT(e.finished == true);
    UASSERT_EQ((size_t)0, chunk.instr_count);  /* no RET emitted when no statements */
    UASSERT_EQ((uint8_t)0, chunk.max_reg);
    uarena_destroy(&arena);
    uchunk_destroy(&chunk);
}

UTEST(uemit_finish_is_idempotent_and_statement_after_finish_returns_finished) {
    Chunk chunk = {0};
    Arena arena;
    uarena_init(&arena, 0);
    Emitter e;
    uemit_init(&e, &chunk, &arena, NULL);
    (void)uemit_finish(&e);
    EmitError second = uemit_finish(&e);
    UASSERT_EQ(EMIT_OK, second);              /* finish is idempotent-OK */
    /* Dummy AST_INT to attempt a statement after finish. */
    AstNode dummy = {0};
    dummy.kind = AST_INT;
    dummy.u.i = 7;
    UASSERT_EQ(EMIT_FINISHED, uemit_statement(&e, &dummy));
    uarena_destroy(&arena);
    uchunk_destroy(&chunk);
}

UTEST(uemit_error_name_returns_sensible_strings) {
    UASSERT_EQ(0, strcmp("EMIT_OK",  uemit_error_name(EMIT_OK)));
    UASSERT_EQ(0, strcmp("EMIT_OOM", uemit_error_name(EMIT_OOM)));
    UASSERT(uemit_error_name(EMIT_UNSUPPORTED_AST) != NULL);
}

/* Helper: drive one statement through init/statement/finish and return the
   resulting EmitError.  chunk and arena are caller-owned; call uchunk_destroy
   and uarena_destroy when done. */
static EmitError emit_single_statement(Chunk *chunk, Arena *arena, AstNode *ast) {
    Emitter e;
    EmitError rc;
    uemit_init(&e, chunk, arena, "test");
    rc = uemit_statement(&e, ast);
    if (rc != EMIT_OK) return rc;
    return uemit_finish(&e);
}

UTEST(emit_ast_int_single_literal_loadk_then_ret) {
    Chunk chunk = {0};
    Arena arena;
    AstNode n = {0};
    uarena_init(&arena, 0);
    n.kind = AST_INT;
    n.u.i  = 42;
    n.line = 1;
    n.col  = 1;

    UASSERT_EQ(EMIT_OK, emit_single_statement(&chunk, &arena, &n));

    /* Two instructions: LOADK R0 K0 ; RET R0 */
    UASSERT_EQ((size_t)2, chunk.instr_count);
    UASSERT_EQ((int)OP_LOADK, (int)uinstr_op(chunk.instructions[0]));
    UASSERT_EQ((uint8_t)0,    uinstr_a(chunk.instructions[0]));
    UASSERT_EQ((uint16_t)0,   uinstr_bx(chunk.instructions[0]));
    UASSERT_EQ((int)OP_RET,   (int)uinstr_op(chunk.instructions[1]));
    UASSERT_EQ((uint8_t)0,    uinstr_a(chunk.instructions[1]));

    /* Constant pool: one UVAL_INT entry, value 42 */
    UASSERT_EQ((size_t)1,      chunk.const_count);
    UASSERT_EQ((uint8_t)UVAL_INT, chunk.constants[0].kind);
    UASSERT_EQ((int64_t)42,    chunk.constants[0].v.i);
    UASSERT_EQ((uint8_t)0,     chunk.max_reg);

    uarena_destroy(&arena);
    uchunk_destroy(&chunk);
}

UTEST(emit_ast_int_dedups_repeated_literal_in_constant_pool) {
    /* Three statements: literal 1, literal 1, literal 2.
       Linear-scan dedup should yield a pool of size 2 (not 3). */
    Chunk chunk = {0};
    Arena arena;
    Emitter e;
    AstNode a = {0};
    AstNode b = {0};
    AstNode c = {0};
    uarena_init(&arena, 0);
    uemit_init(&e, &chunk, &arena, "test");

    a.kind = AST_INT; a.u.i = 1; a.line = 1;
    b.kind = AST_INT; b.u.i = 1; b.line = 1;
    c.kind = AST_INT; c.u.i = 2; c.line = 1;

    UASSERT_EQ(EMIT_OK, uemit_statement(&e, &a));
    UASSERT_EQ(EMIT_OK, uemit_statement(&e, &b));
    UASSERT_EQ(EMIT_OK, uemit_statement(&e, &c));
    UASSERT_EQ(EMIT_OK, uemit_finish(&e));

    /* Pool must have exactly 2 entries: 1 (deduped) and 2. */
    UASSERT_EQ((size_t)2,   chunk.const_count);
    UASSERT_EQ((int64_t)1,  chunk.constants[0].v.i);
    UASSERT_EQ((int64_t)2,  chunk.constants[1].v.i);

    uarena_destroy(&arena);
    uchunk_destroy(&chunk);
}

void test_emit_suite(void);

void test_emit_suite(void) {
    utest_run("uemit_init zeros emitter and does not touch chunk",
              uemit_init_zeros_emitter_and_does_not_touch_chunk);
    utest_run("uemit_finish on empty chunk emits nothing and returns OK",
              uemit_finish_on_empty_chunk_emits_nothing_and_returns_ok);
    utest_run("uemit_finish is idempotent; subsequent statement returns FINISHED",
              uemit_finish_is_idempotent_and_statement_after_finish_returns_finished);
    utest_run("uemit_error_name returns a sensible string",
              uemit_error_name_returns_sensible_strings);
    utest_run("emit AST_INT single literal -> LOADK then RET",
              emit_ast_int_single_literal_loadk_then_ret);
    utest_run("emit AST_INT dedups repeated literal in constant pool",
              emit_ast_int_dedups_repeated_literal_in_constant_pool);
}
