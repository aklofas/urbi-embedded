/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include <stdlib.h>
#include <string.h>

#include "uarena.h"
#include "uemit.h"

#define UTEST(name) static void name(void)

UTEST(uemit_init_zeros_emitter_and_does_not_touch_module) {
    UModule module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    UEmitter e;
    uemit_init(&e, &module, &arena, "repl");

    UASSERT_EQ((uint8_t)0, e.next_reg);
    UASSERT_EQ((uint8_t)0, e.max_reg_seen);
    UASSERT_EQ((uint32_t)0, e.prev_line);
    UASSERT(e.any_stmt_emitted == false);
    UASSERT(e.finished == false);
    UASSERT_EQ(EMIT_OK, e.error);
    UASSERT_EQ((UModule *)&module, e.module);
    UASSERT_EQ((UArena *)&arena, e.arena);
    UASSERT_EQ((size_t)0, module.instr_count);
    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(uemit_finish_on_empty_module_emits_nothing_and_returns_ok) {
    UModule module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    UEmitter e;
    uemit_init(&e, &module, &arena, NULL);
    UEmitError rc = uemit_finish(&e);
    UASSERT_EQ(EMIT_OK, rc);
    UASSERT(e.finished == true);
    UASSERT_EQ((size_t)0, module.instr_count);  /* no RET emitted when no statements */
    UASSERT_EQ((uint8_t)0, module.max_reg);
    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(uemit_finish_is_idempotent_and_statement_after_finish_returns_finished) {
    UModule module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    UEmitter e;
    uemit_init(&e, &module, &arena, NULL);
    (void)uemit_finish(&e);
    UEmitError second = uemit_finish(&e);
    UASSERT_EQ(EMIT_OK, second);              /* finish is idempotent-OK */
    /* Dummy AST_INT to attempt a statement after finish. */
    UAstNode dummy = {0};
    dummy.kind = AST_INT;
    dummy.u.i = 7;
    UASSERT_EQ(EMIT_FINISHED, uemit_statement(&e, &dummy));
    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(uemit_error_name_returns_sensible_strings) {
    UASSERT_EQ(0, strcmp("EMIT_OK",  uemit_error_name(EMIT_OK)));
    UASSERT_EQ(0, strcmp("EMIT_OOM", uemit_error_name(EMIT_OOM)));
    UASSERT(uemit_error_name(EMIT_UNSUPPORTED_AST) != NULL);
}

/* Helper: drive one statement through init/statement/finish and return the
   resulting UEmitError.  module and arena are caller-owned; call umodule_destroy
   and uarena_destroy when done. */
static UEmitError emit_single_statement(UModule *module, UArena *arena, UAstNode *ast) {
    UEmitter e;
    UEmitError rc;
    uemit_init(&e, module, arena, "test");
    rc = uemit_statement(&e, ast);
    if (rc != EMIT_OK) return rc;
    return uemit_finish(&e);
}

UTEST(emit_ast_int_single_literal_loadk_then_ret) {
    UModule module = {0};
    UArena arena;
    UAstNode n = {0};
    uarena_init(&arena, 0);
    n.kind = AST_INT;
    n.u.i  = 42;
    n.line = 1;
    n.col  = 1;

    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &n));

    /* Two instructions: LOADK R0 K0 ; RET R0 */
    UASSERT_EQ((size_t)2, module.instr_count);
    UASSERT_EQ((int)OP_LOADK, (int)uinstr_op(module.instructions[0]));
    UASSERT_EQ((uint8_t)0,    uinstr_a(module.instructions[0]));
    UASSERT_EQ((uint16_t)0,   uinstr_bx(module.instructions[0]));
    UASSERT_EQ((int)OP_RET,   (int)uinstr_op(module.instructions[1]));
    UASSERT_EQ((uint8_t)0,    uinstr_a(module.instructions[1]));

    /* Constant pool: one UVAL_INT entry, value 42 */
    UASSERT_EQ((size_t)1,      module.const_count);
    UASSERT_EQ((uint8_t)UVAL_INT, module.constants[0].kind);
    UASSERT_EQ((int64_t)42,    module.constants[0].v.i);
    UASSERT_EQ((uint8_t)0,     module.max_reg);

    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(emit_ast_int_dedups_repeated_literal_in_constant_pool) {
    /* Three statements: literal 1, literal 1, literal 2.
       Linear-scan dedup should yield a pool of size 2 (not 3). */
    UModule module = {0};
    UArena arena;
    UEmitter e;
    UAstNode a = {0};
    UAstNode b = {0};
    UAstNode c = {0};
    uarena_init(&arena, 0);
    uemit_init(&e, &module, &arena, "test");

    a.kind = AST_INT; a.u.i = 1; a.line = 1;
    b.kind = AST_INT; b.u.i = 1; b.line = 1;
    c.kind = AST_INT; c.u.i = 2; c.line = 1;

    UASSERT_EQ(EMIT_OK, uemit_statement(&e, &a));
    UASSERT_EQ(EMIT_OK, uemit_statement(&e, &b));
    UASSERT_EQ(EMIT_OK, uemit_statement(&e, &c));
    UASSERT_EQ(EMIT_OK, uemit_finish(&e));

    /* Pool must have exactly 2 entries: 1 (deduped) and 2. */
    UASSERT_EQ((size_t)2,   module.const_count);
    UASSERT_EQ((int64_t)1,  module.constants[0].v.i);
    UASSERT_EQ((int64_t)2,  module.constants[1].v.i);

    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(emit_ast_binary_1_plus_2) {
    UModule module = {0};
    UArena arena;
    uarena_init(&arena, 0);

    UAstNode lhs = {0}; lhs.kind = AST_INT; lhs.u.i = 1; lhs.line = 1;
    UAstNode rhs = {0}; rhs.kind = AST_INT; rhs.u.i = 2; rhs.line = 1;
    UAstNode bin = {0};
    bin.kind = AST_BINARY;
    bin.u.binary.op = BOP_ADD;
    bin.u.binary.lhs = &lhs;
    bin.u.binary.rhs = &rhs;
    bin.line = 1;

    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &bin));
    /* LOADK R0 K0 ; LOADK R1 K1 ; ADD R0 R0 R1 ; RET R0 */
    UASSERT_EQ((size_t)4, module.instr_count);
    UASSERT_EQ((int)OP_LOADK, (int)uinstr_op(module.instructions[0]));
    UASSERT_EQ((uint8_t)0, uinstr_a(module.instructions[0]));
    UASSERT_EQ((int)OP_LOADK, (int)uinstr_op(module.instructions[1]));
    UASSERT_EQ((uint8_t)1, uinstr_a(module.instructions[1]));
    UASSERT_EQ((int)OP_ADD, (int)uinstr_op(module.instructions[2]));
    UASSERT_EQ((uint8_t)0, uinstr_a(module.instructions[2]));
    UASSERT_EQ((uint8_t)0, uinstr_b(module.instructions[2]));
    UASSERT_EQ((uint8_t)1, uinstr_c(module.instructions[2]));
    UASSERT_EQ((int)OP_RET, (int)uinstr_op(module.instructions[3]));
    UASSERT_EQ((uint8_t)0, uinstr_a(module.instructions[3]));
    UASSERT_EQ((uint8_t)1, module.max_reg);

    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(emit_ast_binary_sub_mul_div_map_to_correct_opcodes) {
    struct { UAstBinaryOp bop; int expected_op; } cases[] = {
        { BOP_SUB, (int)OP_SUB },
        { BOP_MUL, (int)OP_MUL },
        { BOP_DIV, (int)OP_DIV }
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        UModule module = {0};
        UArena arena;
        uarena_init(&arena, 0);
        UAstNode lhs = {0}; lhs.kind = AST_INT; lhs.u.i = 1; lhs.line = 1;
        UAstNode rhs = {0}; rhs.kind = AST_INT; rhs.u.i = 2; rhs.line = 1;
        UAstNode bin = {0};
        bin.kind = AST_BINARY;
        bin.u.binary.op = cases[i].bop;
        bin.u.binary.lhs = &lhs;
        bin.u.binary.rhs = &rhs;
        bin.line = 1;
        UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &bin));
        UASSERT_EQ(cases[i].expected_op, (int)uinstr_op(module.instructions[2]));
        uarena_destroy(&arena);
        umodule_destroy(&module);
    }
}

UTEST(emit_nested_binary_1_plus_2_plus_3_plus_4_stays_at_max_reg_2) {
    /* (1+2)+(3+4) — 6 UAstNodes.  Destination-reuse keeps max_reg==1. */
    UModule module = {0};
    UArena arena;
    uarena_init(&arena, 0);

    UAstNode a = {0}; a.kind = AST_INT; a.u.i = 1; a.line = 1;
    UAstNode b = {0}; b.kind = AST_INT; b.u.i = 2; b.line = 1;
    UAstNode c = {0}; c.kind = AST_INT; c.u.i = 3; c.line = 1;
    UAstNode d = {0}; d.kind = AST_INT; d.u.i = 4; d.line = 1;
    UAstNode ab = {0};
    ab.kind = AST_BINARY; ab.u.binary.op = BOP_ADD;
    ab.u.binary.lhs = &a; ab.u.binary.rhs = &b; ab.line = 1;
    UAstNode cd = {0};
    cd.kind = AST_BINARY; cd.u.binary.op = BOP_ADD;
    cd.u.binary.lhs = &c; cd.u.binary.rhs = &d; cd.line = 1;
    UAstNode top = {0};
    top.kind = AST_BINARY; top.u.binary.op = BOP_ADD;
    top.u.binary.lhs = &ab; top.u.binary.rhs = &cd; top.line = 1;

    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &top));
    /* Destination-reuse recycles the lhs slot after each ADD, but the rhs
       child still needs its own register simultaneously.  For the two-level
       tree (ab)+(cd) the peak is R2: emitting `d` requires R0(ab-lhs),
       R1(cd-lhs), R2(d) live at once before the inner free_reg. */
    UASSERT_EQ((uint8_t)2, module.max_reg);

    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(emit_ast_unary_neg_5_loadk_then_neg_then_ret) {
    /* AST_UNARY(UOP_NEG, AST_INT 5) -> LOADK R0 K0 ; NEG R0 R0 ; RET R0 */
    UModule module = {0};
    UArena arena;
    uarena_init(&arena, 0);

    UAstNode operand = {0};
    operand.kind = AST_INT;
    operand.u.i  = 5;
    operand.line = 1;

    UAstNode unary = {0};
    unary.kind = AST_UNARY;
    unary.u.unary.op      = UOP_NEG;
    unary.u.unary.operand = &operand;
    unary.line = 1;

    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &unary));

    UASSERT_EQ((size_t)3, module.instr_count);

    UASSERT_EQ((int)OP_LOADK, (int)uinstr_op(module.instructions[0]));
    UASSERT_EQ((uint8_t)0,    uinstr_a(module.instructions[0]));
    UASSERT_EQ((uint16_t)0,   uinstr_bx(module.instructions[0]));

    UASSERT_EQ((int)OP_NEG,   (int)uinstr_op(module.instructions[1]));
    UASSERT_EQ((uint8_t)0,    uinstr_a(module.instructions[1]));
    UASSERT_EQ((uint8_t)0,    uinstr_b(module.instructions[1]));
    UASSERT_EQ((uint8_t)0,    uinstr_c(module.instructions[1]));

    UASSERT_EQ((int)OP_RET,   (int)uinstr_op(module.instructions[2]));
    UASSERT_EQ((uint8_t)0,    uinstr_a(module.instructions[2]));

    UASSERT_EQ((uint8_t)0,    module.max_reg);

    uarena_destroy(&arena);
    umodule_destroy(&module);
}

/* Custom allocator that fails after `fails_after` successful calls.
   Used to drive EMIT_OOM without touching the default stdlib allocator. */
typedef struct { size_t ok_calls; size_t fails_after; } LimitAlloc;

static void *limit_alloc(void *ptr, size_t nbytes, void *ud) {
    LimitAlloc *la = (LimitAlloc *)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    if (la->ok_calls >= la->fails_after) return NULL;
    la->ok_calls++;
    return realloc(ptr, nbytes);
}

UTEST(emit_ast_error_returns_emit_ast_error) {
    UModule module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    UAstNode err = {0};
    err.kind = AST_ERROR;
    err.u.err.code = 1;
    err.u.err.message = "parser error";
    UASSERT_EQ(EMIT_AST_ERROR, emit_single_statement(&module, &arena, &err));
    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(emit_ast_ident_returns_emit_unsupported_ast) {
    UModule module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    UAstNode id = {0};
    id.kind = AST_IDENT;
    id.u.ident.start = "x";
    id.u.ident.len = 1;
    UASSERT_EQ(EMIT_UNSUPPORTED_AST, emit_single_statement(&module, &arena, &id));
    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(emit_first_error_latches_and_subsequent_statements_short_circuit) {
    UModule module = {0};
    UArena arena;
    UEmitter e;
    uarena_init(&arena, 0);
    uemit_init(&e, &module, &arena, NULL);

    UAstNode err = {0};
    err.kind = AST_ERROR;
    err.u.err.code = 1;
    err.u.err.message = "x";
    UASSERT_EQ(EMIT_AST_ERROR, uemit_statement(&e, &err));

    /* Subsequent valid AST_INT still returns the latched error. */
    UAstNode ok = {0};
    ok.kind = AST_INT;
    ok.u.i = 7;
    UASSERT_EQ(EMIT_AST_ERROR, uemit_statement(&e, &ok));

    UASSERT_EQ(EMIT_AST_ERROR, uemit_finish(&e));
    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(emit_emit_oom_when_constant_pool_realloc_fails) {
    UModule module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    LimitAlloc la;
    la.ok_calls = 0;
    la.fails_after = 0;
    module.alloc_fn = limit_alloc;
    module.alloc_ud = &la;

    UAstNode n = {0};
    n.kind = AST_INT;
    n.u.i = 1;
    UASSERT_EQ(EMIT_OOM, emit_single_statement(&module, &arena, &n));
    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(emit_syncline_first_instruction_triggers_abs_line_checkpoint) {
    UModule module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    UAstNode n = {0};
    n.kind = AST_INT; n.u.i = 1; n.line = 10;
    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &n));
    UASSERT_EQ((size_t)2, module.instr_count);  /* LOADK ; RET */
    /* First instruction has INT8_MIN sentinel delta (triggers abs_line lookup). */
    UASSERT_EQ((int8_t)-128, module.line_deltas[0]);
    UASSERT_EQ((size_t)1, module.abs_line_count);
    UASSERT_EQ((uint32_t)0,  module.abs_lines[0].pc);
    UASSERT_EQ((uint32_t)10, module.abs_lines[0].line);
    /* Second instruction (RET) is on the same line, delta 0. */
    UASSERT_EQ((int8_t)0, module.line_deltas[1]);
    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(emit_syncline_small_delta_between_statements_uses_delta_byte) {
    UModule module = {0};
    UArena arena;
    UEmitter e;
    uarena_init(&arena, 0);
    uemit_init(&e, &module, &arena, NULL);

    UAstNode a = {0}; a.kind = AST_INT; a.u.i = 1; a.line = 1;
    UAstNode b = {0}; b.kind = AST_INT; b.u.i = 2; b.line = 3;
    UASSERT_EQ(EMIT_OK, uemit_statement(&e, &a));
    UASSERT_EQ(EMIT_OK, uemit_statement(&e, &b));
    UASSERT_EQ(EMIT_OK, uemit_finish(&e));

    /* Two LOADKs and a RET.  First LOADK at line 1 — abs checkpoint.
       Second LOADK at line 3 — delta = +2 stored inline. */
    UASSERT_EQ((size_t)3, module.instr_count);
    UASSERT_EQ((int8_t)-128, module.line_deltas[0]);
    UASSERT_EQ((int8_t)2,    module.line_deltas[1]);
    UASSERT_EQ((int8_t)0,    module.line_deltas[2]);     /* RET shares line with last instr */
    UASSERT_EQ((size_t)1, module.abs_line_count);
    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(emit_syncline_overflow_triggers_new_abs_line_checkpoint) {
    UModule module = {0};
    UArena arena;
    UEmitter e;
    uarena_init(&arena, 0);
    uemit_init(&e, &module, &arena, NULL);

    UAstNode a = {0}; a.kind = AST_INT; a.u.i = 1; a.line = 1;
    UAstNode b = {0}; b.kind = AST_INT; b.u.i = 2; b.line = 500;  /* delta +499, overflow */
    UASSERT_EQ(EMIT_OK, uemit_statement(&e, &a));
    UASSERT_EQ(EMIT_OK, uemit_statement(&e, &b));
    UASSERT_EQ(EMIT_OK, uemit_finish(&e));

    UASSERT_EQ((size_t)2, module.abs_line_count);
    UASSERT_EQ((uint32_t)1,   module.abs_lines[0].line);
    UASSERT_EQ((uint32_t)500, module.abs_lines[1].line);
    /* pc=0 first abs_line; pc=1 second abs_line (second LOADK). */
    UASSERT_EQ((uint32_t)0, module.abs_lines[0].pc);
    UASSERT_EQ((uint32_t)1, module.abs_lines[1].pc);
    UASSERT_EQ((int8_t)-128, module.line_deltas[1]);        /* sentinel */
    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(disassemble_empty_module_produces_short_placeholder) {
    UModule module = {0};
    UArena arena;
    UEmitter e;
    uarena_init(&arena, 0);
    uemit_init(&e, &module, &arena, NULL);
    (void)uemit_finish(&e);

    char buf[256];
    size_t n = uemit_disassemble(&module, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "(empty)") != NULL || n <= 32);
    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(disassemble_1_plus_2_produces_recognizable_text) {
    UModule module = {0};
    UArena arena;
    UAstNode lhs = {0};
    UAstNode rhs = {0};
    UAstNode bin = {0};
    uarena_init(&arena, 0);
    lhs.kind = AST_INT; lhs.u.i = 1; lhs.line = 1;
    rhs.kind = AST_INT; rhs.u.i = 2; rhs.line = 1;
    bin.kind = AST_BINARY; bin.u.binary.op = BOP_ADD;
    bin.u.binary.lhs = &lhs; bin.u.binary.rhs = &rhs; bin.line = 1;
    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &bin));

    char buf[512];
    size_t n = uemit_disassemble(&module, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "LOADK") != NULL);
    UASSERT(strstr(buf, "ADD")   != NULL);
    UASSERT(strstr(buf, "RET")   != NULL);
    UASSERT(strstr(buf, "R0")    != NULL);
    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(disassemble_truncates_cleanly_when_buf_is_too_small) {
    UModule module = {0};
    UArena arena;
    UAstNode n = {0};
    uarena_init(&arena, 0);
    n.kind = AST_INT; n.u.i = 1; n.line = 1;
    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &n));

    char buf[8];
    size_t written = uemit_disassemble(&module, buf, sizeof buf);
    UASSERT(written < sizeof buf);
    UASSERT_EQ('\0', buf[sizeof buf - 1]);
    uarena_destroy(&arena);
    umodule_destroy(&module);
}

/* --- Additional coverage tests --- */

UTEST(uemit_error_name_covers_all_codes) {
    /* Exercise every UEmitError case in uemit_error_name. */
    UASSERT_EQ(0, strcmp("EMIT_OK",                 uemit_error_name(EMIT_OK)));
    UASSERT_EQ(0, strcmp("EMIT_OOM",                uemit_error_name(EMIT_OOM)));
    UASSERT_EQ(0, strcmp("EMIT_AST_ERROR",          uemit_error_name(EMIT_AST_ERROR)));
    UASSERT_EQ(0, strcmp("EMIT_UNSUPPORTED_AST",    uemit_error_name(EMIT_UNSUPPORTED_AST)));
    UASSERT_EQ(0, strcmp("EMIT_REG_EXHAUSTED",      uemit_error_name(EMIT_REG_EXHAUSTED)));
    UASSERT_EQ(0, strcmp("EMIT_CONSTANT_POOL_FULL", uemit_error_name(EMIT_CONSTANT_POOL_FULL)));
    UASSERT_EQ(0, strcmp("EMIT_LINE_OVERFLOW",      uemit_error_name(EMIT_LINE_OVERFLOW)));
    UASSERT_EQ(0, strcmp("EMIT_FINISHED",           uemit_error_name(EMIT_FINISHED)));
    /* Out-of-range falls through to EMIT_UNKNOWN. */
    UASSERT(uemit_error_name((UEmitError)99) != NULL);
}

UTEST(disassemble_with_neg_instruction_shows_neg) {
    /* Emit a NEG instruction so the OP_NEG case in uemit_disassemble is hit. */
    UModule module = {0};
    UArena arena;
    uarena_init(&arena, 0);

    UAstNode operand = {0};
    operand.kind = AST_INT; operand.u.i = 7; operand.line = 1;
    UAstNode neg = {0};
    neg.kind = AST_UNARY; neg.u.unary.op = UOP_NEG; neg.u.unary.operand = &operand;
    neg.line = 1;

    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &neg));

    char buf[256];
    size_t n = uemit_disassemble(&module, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "NEG") != NULL);

    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(serialize_with_large_constant_exercises_multibyte_varint) {
    /* Use a constant value >= 128 so that uvarint_write_u and uvarint_write_zz
       emit multi-byte (continuation-bit) encoded varints. */
    UModule module = {0};
    UArena arena;
    uarena_init(&arena, 0);

    UAstNode n = {0};
    n.kind = AST_INT; n.u.i = 1000; n.line = 1;  /* 1000 > 63, zigzag = 2000 > 127 */

    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &n));

    /* Serialize and round-trip to confirm multi-byte varint path works. */
    ptrdiff_t need = umodule_serialize(&module, NULL, 0);
    UASSERT((ptrdiff_t)0 < need);

    uint8_t *buf = (uint8_t *)malloc((size_t)need);
    ptrdiff_t wrote = umodule_serialize(&module, buf, (size_t)need);
    UASSERT_EQ(need, wrote);

    UModule dst = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&dst, buf, (size_t)need, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    UASSERT_EQ((size_t)1, dst.const_count);
    UASSERT_EQ((int64_t)1000, dst.constants[0].v.i);

    free(buf);
    uarena_destroy(&arena);
    umodule_destroy(&module);
    umodule_destroy(&dst);
}

UTEST(disassemble_module_with_all_arithmetic_opcodes) {
    /* Emit ADD, SUB, MUL, DIV to exercise all opname() paths.
       Also exercises the "; constants:" section of the disassembler
       which is reached by any instruction module. */
    UModule module = {0};
    UArena arena;
    UEmitter e;
    uarena_init(&arena, 0);
    uemit_init(&e, &module, &arena, NULL);

    /* Each statement emits one binary op. */
    UAstNode lhs = {0}; lhs.kind = AST_INT; lhs.u.i = 1; lhs.line = 1;
    UAstNode rhs = {0}; rhs.kind = AST_INT; rhs.u.i = 2; rhs.line = 1;

    UAstNode sub = {0};
    sub.kind = AST_BINARY; sub.u.binary.op = BOP_SUB;
    sub.u.binary.lhs = &lhs; sub.u.binary.rhs = &rhs; sub.line = 1;
    UASSERT_EQ(EMIT_OK, uemit_statement(&e, &sub));

    UAstNode mul = {0};
    mul.kind = AST_BINARY; mul.u.binary.op = BOP_MUL;
    mul.u.binary.lhs = &lhs; mul.u.binary.rhs = &rhs; mul.line = 1;
    UASSERT_EQ(EMIT_OK, uemit_statement(&e, &mul));

    UAstNode div = {0};
    div.kind = AST_BINARY; div.u.binary.op = BOP_DIV;
    div.u.binary.lhs = &lhs; div.u.binary.rhs = &rhs; div.line = 1;
    UASSERT_EQ(EMIT_OK, uemit_statement(&e, &div));
    UASSERT_EQ(EMIT_OK, uemit_finish(&e));

    char buf[1024];
    size_t n = uemit_disassemble(&module, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "SUB")  != NULL);
    UASSERT(strstr(buf, "MUL")  != NULL);
    UASSERT(strstr(buf, "DIV")  != NULL);
    UASSERT(strstr(buf, "RET")  != NULL);

    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(serialize_module_with_float_constant_round_trips) {
    /* Manually build a module with a UVAL_FLOAT constant and serialize/deserialize
       it to exercise the UVAL_FLOAT branches in module_wire_size and umodule_serialize. */
    UModule module = {0};

    /* Manually insert a UVAL_FLOAT constant (bypassing the emitter, which only
       produces INT constants at M1). */
    module.constants = (UValue *)malloc(sizeof(UValue));
    module.const_cap = 1;
    module.const_count = 1;
    module.constants[0].kind = (uint8_t)UVAL_FLOAT;
    {
        int p;
        for (p = 0; p < 7; p++) module.constants[0]._pad[p] = 0;
    }
#if URBI_FLOAT_TYPE == 8
    module.constants[0].v.f = 2.718281828;
#else
    module.constants[0].v.f = 2.718f;
#endif

    /* Add a RET instruction and synclines so the module is valid. */
    module.instructions = (uint32_t *)malloc(sizeof(uint32_t));
    module.instr_cap = 1;
    module.instr_count = 1;
    module.instructions[0] = uinstr_enc_abc(OP_RET, 0, 0, 0);
    module.line_deltas = (int8_t *)malloc(sizeof(int8_t));
    module.line_deltas[0] = (int8_t)-128;
    module.abs_lines = (UAbsLine *)malloc(sizeof(UAbsLine));
    module.abs_line_cap = 1;
    module.abs_line_count = 1;
    module.abs_lines[0].pc = 0;
    module.abs_lines[0].line = 1;
    module.max_reg = 0;

    ptrdiff_t need = umodule_serialize(&module, NULL, 0);
    UASSERT((ptrdiff_t)0 < need);

    uint8_t *buf = (uint8_t *)malloc((size_t)need);
    ptrdiff_t wrote = umodule_serialize(&module, buf, (size_t)need);
    UASSERT_EQ(need, wrote);

    UModule dst = {0};
    char errmsg[128];
    UModuleLoadError rc = umodule_deserialize(&dst, buf, (size_t)need, errmsg, sizeof errmsg);
    UASSERT_EQ(ULOAD_OK, rc);
    UASSERT_EQ((size_t)1, dst.const_count);
    UASSERT_EQ((uint8_t)UVAL_FLOAT, dst.constants[0].kind);

    /* Disassemble the float module — exercises the ";   K%zu = ?" fallback
       in the constant-pool dump (FLOAT is not UVAL_INT). */
    char disbuf[256];
    size_t disn = uemit_disassemble(&dst, disbuf, sizeof disbuf);
    UASSERT(disn > 0);
    UASSERT(strstr(disbuf, "K0 = ?") != NULL);

    free(buf);
    umodule_destroy(&module);
    umodule_destroy(&dst);
}

UTEST(disassemble_module_with_move_instruction_shows_move) {
    /* OP_MOVE falls through to the default: case in uemit_disassemble,
       calling opname(OP_MOVE) — covers that branch in opname(). */
    UModule module = {0};
    const int64_t consts[] = { 42 };
    const uint32_t instrs[] = {
        uinstr_enc_abx(OP_LOADK, 0, 0),         /* R0 = 42 */
        uinstr_enc_abc(OP_MOVE, 1, 0, 0),       /* R1 = R0 */
        uinstr_enc_abc(OP_RET, 1, 0, 0)
    };
    /* Build a module directly. */
    module.constants  = (UValue *)malloc(sizeof(UValue));
    module.const_cap  = 1; module.const_count = 1;
    module.constants[0].kind = (uint8_t)UVAL_INT;
    {
        int p;
        for (p = 0; p < 7; p++) module.constants[0]._pad[p] = 0;
    }
    module.constants[0].v.i = consts[0];
    module.instructions = (uint32_t *)malloc(sizeof(instrs));
    module.instr_cap = 3; module.instr_count = 3;
    {
        int j;
        for (j = 0; j < 3; j++) module.instructions[j] = instrs[j];
    }
    module.line_deltas = (int8_t *)malloc(3);
    module.line_deltas[0] = (int8_t)-128;
    module.line_deltas[1] = 0;
    module.line_deltas[2] = 0;
    module.abs_lines = (UAbsLine *)malloc(sizeof(UAbsLine));
    module.abs_line_cap = 1; module.abs_line_count = 1;
    module.abs_lines[0].pc = 0; module.abs_lines[0].line = 1;
    module.max_reg = 1;

    char buf[512];
    size_t n = uemit_disassemble(&module, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "MOVE") != NULL);

    umodule_destroy(&module);
}

UTEST(emit_syncline_negative_overflow_triggers_new_abs_line_checkpoint) {
    /* When the line delta is <= INT8_MIN (-128) — i.e. going more than 127
       lines *backward* — a new abs_line checkpoint is emitted instead of
       a delta.  Tests the `d <= INT8_MIN` branch in emit_instr. */
    UModule module = {0};
    UArena arena;
    UEmitter e;
    uarena_init(&arena, 0);
    uemit_init(&e, &module, &arena, NULL);

    UAstNode a = {0}; a.kind = AST_INT; a.u.i = 1; a.line = 500;
    UAstNode b = {0}; b.kind = AST_INT; b.u.i = 2; b.line = 1;  /* delta = 1 - 500 = -499 */
    UASSERT_EQ(EMIT_OK, uemit_statement(&e, &a));
    UASSERT_EQ(EMIT_OK, uemit_statement(&e, &b));
    UASSERT_EQ(EMIT_OK, uemit_finish(&e));

    /* Both statements trigger abs_line checkpoints: first due to "first instruction",
       second due to delta overflow (|delta| > 127). */
    UASSERT_EQ((size_t)2, module.abs_line_count);
    UASSERT_EQ((uint32_t)500, module.abs_lines[0].line);
    UASSERT_EQ((uint32_t)1,   module.abs_lines[1].line);
    UASSERT_EQ((int8_t)-128, module.line_deltas[1]);  /* sentinel on second LOADK */
    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(emit_oom_in_push_abs_line) {
    /* Drive OOM in emit_push_abs_line by allowing the constant-pool alloc
       and the instruction alloc to succeed, then failing the abs_lines alloc.
       The first instruction always triggers emit_push_abs_line.
       Allocation order: (1) constants grow, (2) instructions grow,
       (3) abs_lines grow — fail this one. */
    UModule module = {0};
    UArena arena;
    uarena_init(&arena, 0);

    LimitAlloc la;
    la.ok_calls = 0;
    la.fails_after = 2;                         /* allow 2, fail 3rd (abs_lines) */
    module.alloc_fn = limit_alloc;
    module.alloc_ud = &la;

    UAstNode n = {0};
    n.kind = AST_INT; n.u.i = 1; n.line = 1;
    UEmitError rc = emit_single_statement(&module, &arena, &n);
    UASSERT_EQ(EMIT_OOM, rc);

    uarena_destroy(&arena);
    umodule_destroy(&module);
}

UTEST(emit_oom_in_push_line_delta) {
    /* Drive OOM in emit_push_line_delta by allowing constant-pool, instruction,
       and abs_lines allocs to succeed, then failing the line_deltas alloc.
       Allocation order: (1) constants grow, (2) instructions grow,
       (3) abs_lines grow, (4) line_deltas alloc — fail this one. */
    UModule module = {0};
    UArena arena;
    uarena_init(&arena, 0);

    LimitAlloc la;
    la.ok_calls = 0;
    la.fails_after = 3;                         /* allow 3, fail 4th (line_deltas) */
    module.alloc_fn = limit_alloc;
    module.alloc_ud = &la;

    UAstNode n = {0};
    n.kind = AST_INT; n.u.i = 1; n.line = 1;
    UEmitError rc = emit_single_statement(&module, &arena, &n);
    UASSERT_EQ(EMIT_OOM, rc);

    uarena_destroy(&arena);
    umodule_destroy(&module);
}

void test_emit_suite(void);

void test_emit_suite(void) {
    utest_run("uemit_init zeros emitter and does not touch module",
              uemit_init_zeros_emitter_and_does_not_touch_module);
    utest_run("uemit_finish on empty module emits nothing and returns OK",
              uemit_finish_on_empty_module_emits_nothing_and_returns_ok);
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
              emit_nested_binary_1_plus_2_plus_3_plus_4_stays_at_max_reg_2);
    utest_run("emit AST_UNARY neg 5 -> LOADK R0 K0 ; NEG R0 R0 ; RET R0",
              emit_ast_unary_neg_5_loadk_then_neg_then_ret);
    utest_run("emit AST_ERROR -> EMIT_AST_ERROR",
              emit_ast_error_returns_emit_ast_error);
    utest_run("emit AST_IDENT -> EMIT_UNSUPPORTED_AST (no globals at M1)",
              emit_ast_ident_returns_emit_unsupported_ast);
    utest_run("emit first error latches; subsequent statements short-circuit",
              emit_first_error_latches_and_subsequent_statements_short_circuit);
    utest_run("emit EMIT_OOM when constant-pool realloc fails",
              emit_emit_oom_when_constant_pool_realloc_fails);
    utest_run("emit syncline: first instruction triggers abs_line checkpoint",
              emit_syncline_first_instruction_triggers_abs_line_checkpoint);
    utest_run("emit syncline: small delta between statements uses delta byte",
              emit_syncline_small_delta_between_statements_uses_delta_byte);
    utest_run("emit syncline: overflow triggers new abs_line checkpoint",
              emit_syncline_overflow_triggers_new_abs_line_checkpoint);
    utest_run("disassemble empty module produces a short placeholder",
              disassemble_empty_module_produces_short_placeholder);
    utest_run("disassemble 1 + 2 produces recognizable text",
              disassemble_1_plus_2_produces_recognizable_text);
    utest_run("disassemble truncates cleanly when buf is too small",
              disassemble_truncates_cleanly_when_buf_is_too_small);
    utest_run("uemit_error_name covers all codes",
              uemit_error_name_covers_all_codes);
    utest_run("disassemble with NEG instruction shows NEG",
              disassemble_with_neg_instruction_shows_neg);
    utest_run("serialize with large constant exercises multi-byte varint",
              serialize_with_large_constant_exercises_multibyte_varint);
    utest_run("emit OOM in push_abs_line returns EMIT_OOM",
              emit_oom_in_push_abs_line);
    utest_run("emit OOM in push_line_delta returns EMIT_OOM",
              emit_oom_in_push_line_delta);
    utest_run("disassemble module with all arithmetic opcodes",
              disassemble_module_with_all_arithmetic_opcodes);
    utest_run("serialize module with UVAL_FLOAT constant round-trips",
              serialize_module_with_float_constant_round_trips);
    utest_run("disassemble module with MOVE instruction shows MOVE",
              disassemble_module_with_move_instruction_shows_move);
    utest_run("emit syncline: negative overflow triggers new abs_line checkpoint",
              emit_syncline_negative_overflow_triggers_new_abs_line_checkpoint);
}
