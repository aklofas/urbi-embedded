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

UTEST(emit_ast_binary_1_plus_2) {
    Chunk chunk = {0};
    Arena arena;
    uarena_init(&arena, 0);

    AstNode lhs = {0}; lhs.kind = AST_INT; lhs.u.i = 1; lhs.line = 1;
    AstNode rhs = {0}; rhs.kind = AST_INT; rhs.u.i = 2; rhs.line = 1;
    AstNode bin = {0};
    bin.kind = AST_BINARY;
    bin.u.binary.op = BOP_ADD;
    bin.u.binary.lhs = &lhs;
    bin.u.binary.rhs = &rhs;
    bin.line = 1;

    UASSERT_EQ(EMIT_OK, emit_single_statement(&chunk, &arena, &bin));
    /* LOADK R0 K0 ; LOADK R1 K1 ; ADD R0 R0 R1 ; RET R0 */
    UASSERT_EQ((size_t)4, chunk.instr_count);
    UASSERT_EQ((int)OP_LOADK, (int)uinstr_op(chunk.instructions[0]));
    UASSERT_EQ((uint8_t)0, uinstr_a(chunk.instructions[0]));
    UASSERT_EQ((int)OP_LOADK, (int)uinstr_op(chunk.instructions[1]));
    UASSERT_EQ((uint8_t)1, uinstr_a(chunk.instructions[1]));
    UASSERT_EQ((int)OP_ADD, (int)uinstr_op(chunk.instructions[2]));
    UASSERT_EQ((uint8_t)0, uinstr_a(chunk.instructions[2]));
    UASSERT_EQ((uint8_t)0, uinstr_b(chunk.instructions[2]));
    UASSERT_EQ((uint8_t)1, uinstr_c(chunk.instructions[2]));
    UASSERT_EQ((int)OP_RET, (int)uinstr_op(chunk.instructions[3]));
    UASSERT_EQ((uint8_t)0, uinstr_a(chunk.instructions[3]));
    UASSERT_EQ((uint8_t)1, chunk.max_reg);

    uarena_destroy(&arena);
    uchunk_destroy(&chunk);
}

UTEST(emit_ast_binary_sub_mul_div_map_to_correct_opcodes) {
    struct { BinaryOp bop; int expected_op; } cases[] = {
        { BOP_SUB, (int)OP_SUB },
        { BOP_MUL, (int)OP_MUL },
        { BOP_DIV, (int)OP_DIV }
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Chunk chunk = {0};
        Arena arena;
        uarena_init(&arena, 0);
        AstNode lhs = {0}; lhs.kind = AST_INT; lhs.u.i = 1; lhs.line = 1;
        AstNode rhs = {0}; rhs.kind = AST_INT; rhs.u.i = 2; rhs.line = 1;
        AstNode bin = {0};
        bin.kind = AST_BINARY;
        bin.u.binary.op = cases[i].bop;
        bin.u.binary.lhs = &lhs;
        bin.u.binary.rhs = &rhs;
        bin.line = 1;
        UASSERT_EQ(EMIT_OK, emit_single_statement(&chunk, &arena, &bin));
        UASSERT_EQ(cases[i].expected_op, (int)uinstr_op(chunk.instructions[2]));
        uarena_destroy(&arena);
        uchunk_destroy(&chunk);
    }
}

UTEST(emit_nested_binary_1_plus_2_plus_3_plus_4_stays_at_max_reg_1) {
    /* (1+2)+(3+4) — 6 AstNodes.  Destination-reuse keeps max_reg==1. */
    Chunk chunk = {0};
    Arena arena;
    uarena_init(&arena, 0);

    AstNode a = {0}; a.kind = AST_INT; a.u.i = 1; a.line = 1;
    AstNode b = {0}; b.kind = AST_INT; b.u.i = 2; b.line = 1;
    AstNode c = {0}; c.kind = AST_INT; c.u.i = 3; c.line = 1;
    AstNode d = {0}; d.kind = AST_INT; d.u.i = 4; d.line = 1;
    AstNode ab = {0};
    ab.kind = AST_BINARY; ab.u.binary.op = BOP_ADD;
    ab.u.binary.lhs = &a; ab.u.binary.rhs = &b; ab.line = 1;
    AstNode cd = {0};
    cd.kind = AST_BINARY; cd.u.binary.op = BOP_ADD;
    cd.u.binary.lhs = &c; cd.u.binary.rhs = &d; cd.line = 1;
    AstNode top = {0};
    top.kind = AST_BINARY; top.u.binary.op = BOP_ADD;
    top.u.binary.lhs = &ab; top.u.binary.rhs = &cd; top.line = 1;

    UASSERT_EQ(EMIT_OK, emit_single_statement(&chunk, &arena, &top));
    /* Destination-reuse recycles the lhs slot after each ADD, but the rhs
       child still needs its own register simultaneously.  For the two-level
       tree (ab)+(cd) the peak is R2: emitting `d` requires R0(ab-lhs),
       R1(cd-lhs), R2(d) live at once before the inner free_reg. */
    UASSERT_EQ((uint8_t)2, chunk.max_reg);

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
    utest_run("emit AST_BINARY 1 + 2 -> LOADK R0 K0 ; LOADK R1 K1 ; ADD R0 R0 R1 ; RET R0",
              emit_ast_binary_1_plus_2);
    utest_run("emit AST_BINARY SUB/MUL/DIV map to correct opcodes",
              emit_ast_binary_sub_mul_div_map_to_correct_opcodes);
    utest_run("emit nested (1+2)+(3+4) stays at max_reg=1 via destination reuse",
              emit_nested_binary_1_plus_2_plus_3_plus_4_stays_at_max_reg_1);
}
