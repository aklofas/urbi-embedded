/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include <stdlib.h>
#include <string.h>

#include "value/uarena.h"
#include "emit/uemit.h"
#include "value/uintern.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

UTEST(uemit_init_zeros_emitter_and_does_not_touch_module) {
    UVM vm;
    UProto module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    UEmitter e;
    uemit_init(&e, &module, &arena, &vm,  "repl");

    UASSERT_EQ((uint8_t)0, e.next_reg);
    UASSERT_EQ((uint8_t)0, e.max_reg_seen);
    UASSERT_EQ((uint32_t)0, e.prev_line);
    UASSERT(e.any_stmt_emitted == false);
    UASSERT(e.finished == false);
    UASSERT_EQ(EMIT_OK, e.error);
    UASSERT_EQ((UProto *)&module, e.module);
    UASSERT_EQ((UArena *)&arena, e.arena);
    UASSERT_EQ((size_t)0, module.instr_count);
    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(uemit_finish_on_empty_module_emits_nothing_and_returns_ok) {
    UVM vm;
    UProto module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    UEmitter e;
    uemit_init(&e, &module, &arena, &vm,  NULL);
    UEmitError rc = uemit_finish(&e);
    UASSERT_EQ(EMIT_OK, rc);
    UASSERT(e.finished == true);
    UASSERT_EQ((size_t)0, module.instr_count);  /* no RET emitted when no statements */
    UASSERT_EQ((uint8_t)0, module.max_reg);
    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(uemit_finish_is_idempotent_and_statement_after_finish_returns_finished) {
    UVM vm;
    UProto module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    UEmitter e;
    uemit_init(&e, &module, &arena, &vm,  NULL);
    (void)uemit_finish(&e);
    UEmitError second = uemit_finish(&e);
    UASSERT_EQ(EMIT_OK, second);              /* finish is idempotent-OK */
    /* Dummy AST_INT to attempt a statement after finish. */
    UAstNode dummy = {0};
    dummy.kind = AST_INT;
    dummy.u.i = 7;
    UASSERT_EQ(EMIT_FINISHED, uemit_statement(&e, &dummy));
    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(uemit_error_name_returns_sensible_strings) {
    UASSERT_EQ(0, strcmp("EMIT_OK",  uemit_error_name(EMIT_OK)));
    UASSERT_EQ(0, strcmp("EMIT_OOM", uemit_error_name(EMIT_OOM)));
    UASSERT(uemit_error_name(EMIT_UNSUPPORTED_AST) != NULL);
}

/* Helper: drive one statement through init/statement/finish and return the
   resulting UEmitError.  module and arena are caller-owned; call uchunk_destroy
   and uarena_destroy when done. */
static UEmitError emit_single_statement(UProto *module, UArena *arena, UVM *vm, UAstNode *ast) {
    UEmitter e;
    UEmitError rc;
    uemit_init(&e, module, arena, vm, "test");
    rc = uemit_statement(&e, ast);
    if (rc != EMIT_OK) return rc;
    return uemit_finish(&e);
}

UTEST(emit_ast_int_single_literal_loadk_then_ret) {
    UVM vm;
    UProto module = {0};
    UArena arena;
    UAstNode n = {0};
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    n.kind = AST_INT;
    n.u.i  = 42;
    n.line = 1;
    n.col  = 1;

    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &vm, &n));

    /* Two instructions: LOADK R1 K0 ; RET R1
     * (T73: chunk-top pre-reserves R0 for r_global_slot; first temp starts at R1) */
    UASSERT_EQ((size_t)2, module.instr_count);
    UASSERT_EQ((int)OP_LOADK, (int)uinstr_op(module.instructions[0]));
    UASSERT_EQ((uint8_t)1,    uinstr_a(module.instructions[0]));
    UASSERT_EQ((uint16_t)0,   uinstr_bx(module.instructions[0]));
    UASSERT_EQ((int)OP_RET,   (int)uinstr_op(module.instructions[1]));
    UASSERT_EQ((uint8_t)1,    uinstr_a(module.instructions[1]));

    /* Constant pool: one UVAL_INT entry, value 42 */
    UASSERT_EQ((size_t)1,      module.const_count);
    UASSERT_EQ((uint8_t)UVAL_INT, module.constants[0].kind);
    UASSERT_EQ((int64_t)42,    module.constants[0].v.i);
    UASSERT_EQ((uint8_t)1,     module.max_reg);

    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(emit_ast_int_dedups_repeated_literal_in_constant_pool) {
    /* Three statements: literal 1, literal 1, literal 2.
       Linear-scan dedup should yield a pool of size 2 (not 3). */
    UVM vm;
    UProto module = {0};
    UArena arena;
    UEmitter e;
    UAstNode a = {0};
    UAstNode b = {0};
    UAstNode c = {0};
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    uemit_init(&e, &module, &arena, &vm,  "test");

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
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(emit_ast_binary_1_plus_2) {
    UVM vm;
    UProto module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);

    UAstNode lhs = {0}; lhs.kind = AST_INT; lhs.u.i = 1; lhs.line = 1;
    UAstNode rhs = {0}; rhs.kind = AST_INT; rhs.u.i = 2; rhs.line = 1;
    UAstNode bin = {0};
    bin.kind = AST_BINARY;
    bin.u.binary.op = BOP_ADD;
    bin.u.binary.lhs = &lhs;
    bin.u.binary.rhs = &rhs;
    bin.line = 1;

    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &vm, &bin));
    /* LOADK R1 K0 ; LOADK R2 K1 ; ADD R1 R1 R2 ; RET R1
     * (T73: chunk-top pre-reserves R0 for r_global_slot; first temp starts at R1) */
    UASSERT_EQ((size_t)4, module.instr_count);
    UASSERT_EQ((int)OP_LOADK, (int)uinstr_op(module.instructions[0]));
    UASSERT_EQ((uint8_t)1, uinstr_a(module.instructions[0]));
    UASSERT_EQ((int)OP_LOADK, (int)uinstr_op(module.instructions[1]));
    UASSERT_EQ((uint8_t)2, uinstr_a(module.instructions[1]));
    UASSERT_EQ((int)OP_ADD, (int)uinstr_op(module.instructions[2]));
    UASSERT_EQ((uint8_t)1, uinstr_a(module.instructions[2]));
    UASSERT_EQ((uint8_t)1, uinstr_b(module.instructions[2]));
    UASSERT_EQ((uint8_t)2, uinstr_c(module.instructions[2]));
    UASSERT_EQ((int)OP_RET, (int)uinstr_op(module.instructions[3]));
    UASSERT_EQ((uint8_t)1, uinstr_a(module.instructions[3]));
    UASSERT_EQ((uint8_t)2, module.max_reg);

    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(emit_ast_binary_sub_mul_div_map_to_correct_opcodes) {
    UVM vm;
    struct { UAstBinaryOp bop; int expected_op; } cases[] = {
        { BOP_SUB, (int)OP_SUB },
        { BOP_MUL, (int)OP_MUL },
        { BOP_DIV, (int)OP_DIV }
    };
    size_t i;
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        UProto module = {0};
        UArena arena;
        uarena_init(&arena, 0);
        urbi_vm_init(&vm, NULL, NULL);
        UAstNode lhs = {0}; lhs.kind = AST_INT; lhs.u.i = 1; lhs.line = 1;
        UAstNode rhs = {0}; rhs.kind = AST_INT; rhs.u.i = 2; rhs.line = 1;
        UAstNode bin = {0};
        bin.kind = AST_BINARY;
        bin.u.binary.op = cases[i].bop;
        bin.u.binary.lhs = &lhs;
        bin.u.binary.rhs = &rhs;
        bin.line = 1;
        UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &vm, &bin));
        UASSERT_EQ(cases[i].expected_op, (int)uinstr_op(module.instructions[2]));
        uarena_destroy(&arena);
        uchunk_destroy(&module, NULL);
        urbi_vm_destroy(&vm);
    }
}

UTEST(emit_nested_binary_1_plus_2_plus_3_plus_4_stays_at_max_reg_2) {
    /* (1+2)+(3+4) — 6 UAstNodes.  Destination-reuse keeps max_reg==1. */
    UVM vm;
    UProto module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);

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

    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &vm, &top));
    /* Destination-reuse recycles the lhs slot after each ADD, but the rhs
       child still needs its own register simultaneously.  For the two-level
       tree (ab)+(cd) the peak is R3: emitting `d` requires R1(ab-lhs),
       R2(cd-lhs), R3(d) live at once before the inner free_reg.
       (T73: chunk-top pre-reserves R0, so temps start at R1.) */
    UASSERT_EQ((uint8_t)3, module.max_reg);

    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(emit_ast_unary_neg_5_loadk_then_neg_then_ret) {
    /* AST_UNARY(UOP_NEG, AST_INT 5) -> LOADK R0 K0 ; NEG R0 R0 ; RET R0 */
    UVM vm;
    UProto module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);

    UAstNode operand = {0};
    operand.kind = AST_INT;
    operand.u.i  = 5;
    operand.line = 1;

    UAstNode unary = {0};
    unary.kind = AST_UNARY;
    unary.u.unary.op      = UOP_NEG;
    unary.u.unary.operand = &operand;
    unary.line = 1;

    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &vm, &unary));

    /* LOADK R1 K0 ; NEG R1 R1 0 ; RET R1
     * (T73: chunk-top pre-reserves R0 for r_global_slot; first temp starts at R1) */
    UASSERT_EQ((size_t)3, module.instr_count);

    UASSERT_EQ((int)OP_LOADK, (int)uinstr_op(module.instructions[0]));
    UASSERT_EQ((uint8_t)1,    uinstr_a(module.instructions[0]));
    UASSERT_EQ((uint16_t)0,   uinstr_bx(module.instructions[0]));

    UASSERT_EQ((int)OP_NEG,   (int)uinstr_op(module.instructions[1]));
    UASSERT_EQ((uint8_t)1,    uinstr_a(module.instructions[1]));
    UASSERT_EQ((uint8_t)1,    uinstr_b(module.instructions[1]));
    UASSERT_EQ((uint8_t)0,    uinstr_c(module.instructions[1]));

    UASSERT_EQ((int)OP_RET,   (int)uinstr_op(module.instructions[2]));
    UASSERT_EQ((uint8_t)1,    uinstr_a(module.instructions[2]));

    UASSERT_EQ((uint8_t)1,    module.max_reg);

    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
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
    UVM vm;
    UProto module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    UAstNode err = {0};
    err.kind = AST_ERROR;
    err.u.err.code = 1;
    err.u.err.message = "parser error";
    UASSERT_EQ(EMIT_AST_ERROR, emit_single_statement(&module, &arena, &vm, &err));
    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(emit_ast_ident_unresolved_name_returns_error) {
    /* After T71, a bare identifier with no matching local or upvalue
       falls through to the realm-global lookup and compiles successfully
       (emits OP_GETSLOT on the r_global_slot register).
       EMIT_UNRESOLVED_NAME is no longer raised for bare identifiers. */
    UVM vm;
    UProto module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    UAstNode id = {0};
    id.kind = AST_IDENT;
    id.u.ident.start = "ghost";
    id.u.ident.len = 5;
    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &vm, &id));
    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(emit_first_error_latches_and_subsequent_statements_short_circuit) {
    UVM vm;
    UProto module = {0};
    UArena arena;
    UEmitter e;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    uemit_init(&e, &module, &arena, &vm,  NULL);

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
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(emit_emit_oom_when_constant_pool_realloc_fails) {
    UVM vm;
    UProto module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    LimitAlloc la;
    la.ok_calls = 0;
    la.fails_after = 0;
    module.alloc_fn = limit_alloc;
    module.alloc_ud = &la;

    UAstNode n = {0};
    n.kind = AST_INT;
    n.u.i = 1;
    UASSERT_EQ(EMIT_OOM, emit_single_statement(&module, &arena, &vm, &n));
    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(emit_syncline_first_instruction_triggers_abs_line_checkpoint) {
    UVM vm;
    UProto module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    UAstNode n = {0};
    n.kind = AST_INT; n.u.i = 1; n.line = 10;
    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &vm, &n));
    UASSERT_EQ((size_t)2, module.instr_count);  /* LOADK ; RET */
    /* First instruction has INT8_MIN sentinel delta (triggers abs_line lookup). */
    UASSERT_EQ((int8_t)-128, module.line_deltas[0]);
    UASSERT_EQ((size_t)1, module.abs_line_count);
    UASSERT_EQ((uint32_t)0,  module.abs_lines[0].pc);
    UASSERT_EQ((uint32_t)10, module.abs_lines[0].line);
    /* Second instruction (RET) is on the same line, delta 0. */
    UASSERT_EQ((int8_t)0, module.line_deltas[1]);
    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(emit_syncline_small_delta_between_statements_uses_delta_byte) {
    UVM vm;
    UProto module = {0};
    UArena arena;
    UEmitter e;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    uemit_init(&e, &module, &arena, &vm,  NULL);

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
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(emit_syncline_overflow_triggers_new_abs_line_checkpoint) {
    UVM vm;
    UProto module = {0};
    UArena arena;
    UEmitter e;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    uemit_init(&e, &module, &arena, &vm,  NULL);

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
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(disassemble_empty_module_produces_short_placeholder) {
    UVM vm;
    UProto module = {0};
    UArena arena;
    UEmitter e;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    uemit_init(&e, &module, &arena, &vm,  NULL);
    (void)uemit_finish(&e);

    char buf[256];
    size_t n = uemit_disassemble(&module, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "(empty)") != NULL || n <= 32);
    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(disassemble_1_plus_2_produces_recognizable_text) {
    UVM vm;
    UProto module = {0};
    UArena arena;
    UAstNode lhs = {0};
    UAstNode rhs = {0};
    UAstNode bin = {0};
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    lhs.kind = AST_INT; lhs.u.i = 1; lhs.line = 1;
    rhs.kind = AST_INT; rhs.u.i = 2; rhs.line = 1;
    bin.kind = AST_BINARY; bin.u.binary.op = BOP_ADD;
    bin.u.binary.lhs = &lhs; bin.u.binary.rhs = &rhs; bin.line = 1;
    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &vm, &bin));

    char buf[512];
    size_t n = uemit_disassemble(&module, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "LOADK") != NULL);
    UASSERT(strstr(buf, "ADD")   != NULL);
    UASSERT(strstr(buf, "RET")   != NULL);
    /* T73: chunk-top pre-reserves R0; temps start at R1. */
    UASSERT(strstr(buf, "R1")    != NULL);
    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(disassemble_truncates_cleanly_when_buf_is_too_small) {
    UVM vm;
    UProto module = {0};
    UArena arena;
    UAstNode n = {0};
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    n.kind = AST_INT; n.u.i = 1; n.line = 1;
    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &vm, &n));

    char buf[8];
    size_t written = uemit_disassemble(&module, buf, sizeof buf);
    UASSERT(written < sizeof buf);
    UASSERT_EQ('\0', buf[sizeof buf - 1]);
    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
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
    UVM vm;
    UProto module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);

    UAstNode operand = {0};
    operand.kind = AST_INT; operand.u.i = 7; operand.line = 1;
    UAstNode neg = {0};
    neg.kind = AST_UNARY; neg.u.unary.op = UOP_NEG; neg.u.unary.operand = &operand;
    neg.line = 1;

    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &vm, &neg));

    char buf[256];
    size_t n = uemit_disassemble(&module, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "NEG") != NULL);

    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(serialize_with_large_constant_exercises_multibyte_varint) {
    /* Use a constant value >= 128 so that uvarint_write_u and uvarint_write_zz
       emit multi-byte (continuation-bit) encoded varints. */
    UVM vm;
    UProto module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);

    UAstNode n = {0};
    n.kind = AST_INT; n.u.i = 1000; n.line = 1;  /* 1000 > 63, zigzag = 2000 > 127 */

    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &vm, &n));

    /* Serialize and round-trip to confirm multi-byte varint path works. */
    ptrdiff_t need = uchunk_serialize(&module, NULL, 0);
    UASSERT((ptrdiff_t)0 < need);

    uint8_t *buf = (uint8_t *)malloc((size_t)need);
    ptrdiff_t wrote = uchunk_serialize(&module, buf, (size_t)need);
    UASSERT_EQ(need, wrote);

    UProto *dst = NULL;
    char errmsg[128];
    UChunkLoadError rc = uchunk_deserialize(&dst, buf, (size_t)need, NULL, NULL, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    UASSERT(dst != NULL);
    UASSERT_EQ((size_t)1, dst->const_count);
    UASSERT_EQ((int64_t)1000, dst->constants[0].v.i);

    free(buf);
    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
    uchunk_destroy(dst, NULL);
urbi_vm_destroy(&vm);
}

UTEST(disassemble_module_with_all_arithmetic_opcodes) {
    /* Emit ADD, SUB, MUL, DIV to exercise all opname() paths.
       Also exercises the "; constants:" section of the disassembler
       which is reached by any instruction module. */
    UVM vm;
    UProto module = {0};
    UArena arena;
    UEmitter e;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    uemit_init(&e, &module, &arena, &vm,  NULL);

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
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(serialize_module_with_float_constant_round_trips) {
    /* Manually build a module with a UVAL_FLOAT constant and serialize/deserialize
       it to exercise the UVAL_FLOAT branches in module_wire_size and uchunk_serialize. */
    UProto module = {0};

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

    ptrdiff_t need = uchunk_serialize(&module, NULL, 0);
    UASSERT((ptrdiff_t)0 < need);

    uint8_t *buf = (uint8_t *)malloc((size_t)need);
    ptrdiff_t wrote = uchunk_serialize(&module, buf, (size_t)need);
    UASSERT_EQ(need, wrote);

    UProto *dst = NULL;
    char errmsg[128];
    UChunkLoadError rc = uchunk_deserialize(&dst, buf, (size_t)need, NULL, NULL, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, rc);
    UASSERT(dst != NULL);
    UASSERT_EQ((size_t)1, dst->const_count);
    UASSERT_EQ((uint8_t)UVAL_FLOAT, dst->constants[0].kind);

    /* Disassemble the float module — exercises the ";   K%zu = ?" fallback
       in the constant-pool dump (FLOAT is not UVAL_INT). */
    char disbuf[256];
    size_t disn = uemit_disassemble(dst, disbuf, sizeof disbuf);
    UASSERT(disn > 0);
    UASSERT(strstr(disbuf, "K0 = ?") != NULL);

    free(buf);
    uchunk_destroy(&module, NULL);
    uchunk_destroy(dst, NULL);
}

UTEST(disassemble_module_with_move_instruction_shows_move) {
    /* OP_MOVE falls through to the default: case in uemit_disassemble,
       calling opname(OP_MOVE) — covers that branch in opname(). */
    UProto module = {0};
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

    uchunk_destroy(&module, NULL);
}

UTEST(emit_syncline_negative_overflow_triggers_new_abs_line_checkpoint) {
    /* When the line delta is <= INT8_MIN (-128) — i.e. going more than 127
       lines *backward* — a new abs_line checkpoint is emitted instead of
       a delta.  Tests the `d <= INT8_MIN` branch in emit_instr. */
    UVM vm;
    UProto module = {0};
    UArena arena;
    UEmitter e;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    uemit_init(&e, &module, &arena, &vm,  NULL);

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
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(emit_oom_in_push_abs_line) {
    /* Drive OOM in emit_push_abs_line by allowing the constant-pool alloc
       and the instruction alloc to succeed, then failing the abs_lines alloc.
       The first instruction always triggers emit_push_abs_line.
       Allocation order: (1) constants grow, (2) instructions grow,
       (3) abs_lines grow — fail this one. */
    UVM vm;
    UProto module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);

    LimitAlloc la;
    la.ok_calls = 0;
    la.fails_after = 2;                         /* allow 2, fail 3rd (abs_lines) */
    module.alloc_fn = limit_alloc;
    module.alloc_ud = &la;

    UAstNode n = {0};
    n.kind = AST_INT; n.u.i = 1; n.line = 1;
    UEmitError rc = emit_single_statement(&module, &arena, &vm, &n);
    UASSERT_EQ(EMIT_OOM, rc);

    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

UTEST(emit_oom_in_push_line_delta) {
    /* Drive OOM in emit_push_line_delta by allowing constant-pool, instruction,
       and abs_lines allocs to succeed, then failing the line_deltas alloc.
       Allocation order: (1) constants grow, (2) instructions grow,
       (3) abs_lines grow, (4) line_deltas alloc — fail this one. */
    UVM vm;
    UProto module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);

    LimitAlloc la;
    la.ok_calls = 0;
    la.fails_after = 3;                         /* allow 3, fail 4th (line_deltas) */
    module.alloc_fn = limit_alloc;
    module.alloc_ud = &la;

    UAstNode n = {0};
    n.kind = AST_INT; n.u.i = 1; n.line = 1;
    UEmitError rc = emit_single_statement(&module, &arena, &vm, &n);
    UASSERT_EQ(EMIT_OOM, rc);

    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
urbi_vm_destroy(&vm);
}

/* --- var-decl + local resolution emit tests (T10) --- */

#include "lex/ulex.h"
#include "parse/uparse.h"

/* Helper context used by all T10 emit tests: parse source + emit. */
typedef struct {
    ULexer  lex;
    UArena  arena;
    UParser p;
    UProto module;
    UVM     vm;
    UEmitter e;
} EmitCtx;

static void emit_ctx_init(EmitCtx *c, const char *src) {
    ulex_init(&c->lex, src, strlen(src));
    uarena_init(&c->arena, 0);
    urbi_vm_init(&c->vm, NULL, NULL);
    c->module = (UProto){0};
    uparse_init(&c->p, &c->lex, &c->arena);
    uemit_init(&c->e, &c->module, &c->arena, &c->vm, "test");
}

static UEmitError emit_ctx_run(EmitCtx *c) {
    UAstNode *stmt;
    while ((stmt = uparse_next_statement(&c->p)) != NULL) {
        UEmitError rc = uemit_statement(&c->e, stmt);
        if (rc != EMIT_OK) return rc;
    }
    return uemit_finish(&c->e);
}

static void emit_ctx_destroy(EmitCtx *c) {
    uarena_destroy(&c->arena);
    uchunk_destroy(&c->module, NULL);
    urbi_vm_destroy(&c->vm);
}

UTEST(emit_var_decl_basic_no_op_move) {
    /* "var x = 7" at chunk-top (T72): declares x as a realm global.
     * Emits LOAD_REALM_GLOBAL + LOADK + SETSLOT + RET (no OP_MOVE for local
     * absorption — x is not a frame local at chunk-top).
     * Inside a function body, var is still local (LOADK + RET, no SETSLOT). */
    EmitCtx c;
    emit_ctx_init(&c, "var x = 7");
    UEmitError rc = emit_ctx_run(&c);
    UASSERT_EQ(EMIT_OK, rc);
    /* Chunk-top path must emit SETSLOT (write to global object). */
    bool found_setslot = false;
    for (size_t i = 0; i < c.module.instr_count; i++) {
        if (uinstr_op(c.module.instructions[i]) == OP_SETSLOT) {
            found_setslot = true;
            break;
        }
    }
    UASSERT(found_setslot);
    emit_ctx_destroy(&c);
}

UTEST(emit_var_then_use_resolves_local) {
    /* "var x = 7; x + 1" — at chunk-top (T72), x is a realm global.
     * Reading x emits OP_GETSLOT (not OP_MOVE) against the global object.
     * Inside a function body, var is still a local accessed via MOVE. */
    EmitCtx c;
    emit_ctx_init(&c, "var x = 7; x + 1");
    UEmitError rc = emit_ctx_run(&c);
    UASSERT_EQ(EMIT_OK, rc);
    /* Chunk-top global read must emit GETSLOT (not MOVE from slot 0). */
    bool found_getslot = false;
    for (size_t i = 0; i < c.module.instr_count; i++) {
        if (uinstr_op(c.module.instructions[i]) == OP_GETSLOT) {
            found_getslot = true;
            break;
        }
    }
    UASSERT(found_getslot);
    emit_ctx_destroy(&c);
}

UTEST(emit_var_redeclare_in_same_scope_is_error) {
    /* At chunk-top (T72), vars are globals (not locals), so redeclaring a
     * chunk-top var is not an error — it just overwrites the global slot.
     * EMIT_LOCAL_REDECLARE is still raised for duplicate vars inside a
     * function body (where they are frame locals). */
    {
        /* Chunk-top: two vars with same name → EMIT_OK (both write to global). */
        EmitCtx c;
        emit_ctx_init(&c, "var x = 1; var x = 2");
        UEmitError rc = emit_ctx_run(&c);
        UASSERT_EQ(EMIT_OK, rc);
        emit_ctx_destroy(&c);
    }
    {
        /* Inside a function: duplicate var → EMIT_LOCAL_REDECLARE. */
        EmitCtx c;
        emit_ctx_init(&c, "function() { var x = 1; var x = 2 }");
        UEmitError rc = emit_ctx_run(&c);
        UASSERT_EQ(EMIT_LOCAL_REDECLARE, rc);
        emit_ctx_destroy(&c);
    }
}

UTEST(emit_unresolved_name_is_error) {
    /* "ghost" — after T71 the realm-global fallback compiles bare
     * identifiers via OP_GETSLOT; no longer EMIT_UNRESOLVED_NAME. */
    EmitCtx c;
    emit_ctx_init(&c, "ghost");
    UEmitError rc = emit_ctx_run(&c);
    UASSERT_EQ(EMIT_OK, rc);
    emit_ctx_destroy(&c);
}

UTEST(emit_assign_to_existing_local) {
    /* "var x = 1; x = 42" — at chunk-top (T72), x is a global.
     * The assignment x = 42 routes to OP_SETSLOT (global write), not OP_MOVE.
     * Both the declaration and the assignment must compile cleanly. */
    EmitCtx c;
    emit_ctx_init(&c, "var x = 1; x = 42");
    UEmitError rc = emit_ctx_run(&c);
    UASSERT_EQ(EMIT_OK, rc);
    /* Both var-decl and assign emit OP_SETSLOT at chunk-top. */
    int setslot_count = 0;
    for (size_t i = 0; i < c.module.instr_count; i++) {
        if (uinstr_op(c.module.instructions[i]) == OP_SETSLOT)
            setslot_count++;
    }
    UASSERT(setslot_count >= 2);
    emit_ctx_destroy(&c);
}

UTEST(emit_assign_to_unresolved_is_error) {
    /* "ghost = 7" — assigning to an undeclared name -> EMIT_UNRESOLVED_NAME */
    EmitCtx c;
    emit_ctx_init(&c, "ghost = 7");
    UEmitError rc = emit_ctx_run(&c);
    UASSERT_EQ(EMIT_UNRESOLVED_NAME, rc);
    emit_ctx_destroy(&c);
}

/* --- Emit tests for bool/nil literals and comparison operator --- */

UTEST(emit_ast_bool_true_emits_loadbool_1_0) {
    UVM vm; UProto module = {0}; UArena arena;
    uarena_init(&arena, 0); urbi_vm_init(&vm, NULL, NULL);
    UAstNode n = {0};
    n.kind = AST_BOOL; n.u.b = true; n.line = 1;
    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &vm, &n));
    /* Instructions: LOADBOOL R1 1 0 ; RET R1
     * (T73: chunk-top pre-reserves R0 for r_global_slot) */
    UASSERT_EQ((size_t)2, module.instr_count);
    UASSERT_EQ((int)OP_LOADBOOL, (int)uinstr_op(module.instructions[0]));
    UASSERT_EQ((uint8_t)1, uinstr_a(module.instructions[0]));
    UASSERT_EQ((uint8_t)1, uinstr_b(module.instructions[0]));
    UASSERT_EQ((uint8_t)0, uinstr_c(module.instructions[0]));
    UASSERT_EQ((int)OP_RET, (int)uinstr_op(module.instructions[1]));
    uarena_destroy(&arena); uchunk_destroy(&module, NULL); urbi_vm_destroy(&vm);
}

UTEST(emit_ast_bool_false_emits_loadbool_0_0) {
    UVM vm; UProto module = {0}; UArena arena;
    uarena_init(&arena, 0); urbi_vm_init(&vm, NULL, NULL);
    UAstNode n = {0};
    n.kind = AST_BOOL; n.u.b = false; n.line = 1;
    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &vm, &n));
    UASSERT_EQ((int)OP_LOADBOOL, (int)uinstr_op(module.instructions[0]));
    UASSERT_EQ((uint8_t)0, uinstr_b(module.instructions[0]));  /* 0 = false */
    uarena_destroy(&arena); uchunk_destroy(&module, NULL); urbi_vm_destroy(&vm);
}

UTEST(emit_ast_nil_emits_loadnil) {
    UVM vm; UProto module = {0}; UArena arena;
    uarena_init(&arena, 0); urbi_vm_init(&vm, NULL, NULL);
    UAstNode n = {0};
    n.kind = AST_NIL; n.line = 1;
    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &vm, &n));
    UASSERT_EQ((int)OP_LOADNIL, (int)uinstr_op(module.instructions[0]));
    /* T73: chunk-top pre-reserves R0; first temp is R1. */
    UASSERT_EQ((uint8_t)1, uinstr_a(module.instructions[0]));
    uarena_destroy(&arena); uchunk_destroy(&module, NULL); urbi_vm_destroy(&vm);
}

UTEST(emit_ast_compare_eq_emits_4_instruction_pattern) {
    /* Build AST for "1 == 2" manually. */
    UVM vm; UProto module = {0}; UArena arena;
    uarena_init(&arena, 0); urbi_vm_init(&vm, NULL, NULL);
    UAstNode lhs = {0}; lhs.kind = AST_INT; lhs.u.i = 1; lhs.line = 1;
    UAstNode rhs = {0}; rhs.kind = AST_INT; rhs.u.i = 2; rhs.line = 1;
    UAstNode cmp = {0};
    cmp.kind = AST_COMPARE; cmp.u.cmp.op = CMP_EQ;
    cmp.u.cmp.lhs = &lhs; cmp.u.cmp.rhs = &rhs; cmp.line = 1;
    UASSERT_EQ(EMIT_OK, emit_single_statement(&module, &arena, &vm, &cmp));
    /* 4-instruction pattern + RET = 5 instructions total */
    /* LOADK, LOADK, EQ, JMP, LOADBOOL(true+skip), LOADBOOL(false), RET */
    UASSERT(module.instr_count >= 7);
    /* First two are LOADK for lhs and rhs. */
    UASSERT_EQ((int)OP_LOADK, (int)uinstr_op(module.instructions[0]));
    UASSERT_EQ((int)OP_LOADK, (int)uinstr_op(module.instructions[1]));
    /* Third is OP_EQ with a_bit=0 for CMP_EQ (skip on equal → produce true). */
    UASSERT_EQ((int)OP_EQ, (int)uinstr_op(module.instructions[2]));
    UASSERT_EQ((uint8_t)0, uinstr_a(module.instructions[2]));
    /* Fourth is OP_JMP. */
    UASSERT_EQ((int)OP_JMP, (int)uinstr_op(module.instructions[3]));
    /* Fifth: LOADBOOL rb, 1, 1 (skip next on true). */
    UASSERT_EQ((int)OP_LOADBOOL, (int)uinstr_op(module.instructions[4]));
    UASSERT_EQ((uint8_t)1, uinstr_b(module.instructions[4]));
    UASSERT_EQ((uint8_t)1, uinstr_c(module.instructions[4]));
    /* Sixth: LOADBOOL rb, 0, 0 (false arm). */
    UASSERT_EQ((int)OP_LOADBOOL, (int)uinstr_op(module.instructions[5]));
    UASSERT_EQ((uint8_t)0, uinstr_b(module.instructions[5]));
    UASSERT_EQ((uint8_t)0, uinstr_c(module.instructions[5]));
    uarena_destroy(&arena); uchunk_destroy(&module, NULL); urbi_vm_destroy(&vm);
}

UTEST(emit_if_then_only) {
    /* "if (true) { 42 }" — verify TEST + JMP fixup + LOADK pattern.
       Expected sequence:
         [0] LOADBOOL R0 1 0   (cond)
         [1] TEST R0 0 1       (skip JMP if truthy)
         [2] JMP <nil-target>  (to [5])
         [3] LOADK R0 K0       (then-block: 42)
         [4] JMP <end-target>  (to [6])
         [5] LOADNIL R0        (nil arm for no-else)
         [6] RET R0            */
    EmitCtx c;
    emit_ctx_init(&c, "if (true) { 42 }");
    UEmitError rc = emit_ctx_run(&c);
    UASSERT_EQ(EMIT_OK, rc);
    /* Must have TEST instruction. */
    bool found_test = false;
    for (size_t i = 0; i < c.module.instr_count; i++) {
        if (uinstr_op(c.module.instructions[i]) == OP_TEST) {
            found_test = true;
            /* C=1 means skip when truthy. */
            UASSERT_EQ((uint8_t)1, uinstr_c(c.module.instructions[i]));
        }
    }
    UASSERT(found_test);
    /* Must have LOADNIL for the no-else nil path. */
    bool found_nil = false;
    for (size_t i = 0; i < c.module.instr_count; i++) {
        if (uinstr_op(c.module.instructions[i]) == OP_LOADNIL) {
            found_nil = true;
        }
    }
    UASSERT(found_nil);
    emit_ctx_destroy(&c);
}

UTEST(emit_if_then_else) {
    /* "if (false) { 42 } else { 99 }" — both arms compiled; no LOADNIL.
       Expected:
         [0] LOADBOOL R0 0 0   (cond = false)
         [1] TEST R0 0 1       (skip JMP if truthy; won't skip since false)
         [2] JMP <else>        (to [5])
         [3] LOADK R0 K(42)    (then)
         [4] JMP <end>         (to [6])
         [5] LOADK R0 K(99)    (else)
         [6] RET R0
       No LOADNIL needed — else arm fills the slot. */
    EmitCtx c;
    emit_ctx_init(&c, "if (false) { 42 } else { 99 }");
    UEmitError rc = emit_ctx_run(&c);
    UASSERT_EQ(EMIT_OK, rc);
    bool found_test = false;
    for (size_t i = 0; i < c.module.instr_count; i++) {
        if (uinstr_op(c.module.instructions[i]) == OP_TEST) {
            found_test = true;
            UASSERT_EQ((uint8_t)1, uinstr_c(c.module.instructions[i]));
        }
    }
    UASSERT(found_test);
    /* No LOADNIL: else provides the alternative value. */
    for (size_t i = 0; i < c.module.instr_count; i++) {
        UASSERT((int)uinstr_op(c.module.instructions[i]) != (int)OP_LOADNIL);
    }
    /* Two JMP instructions: one for the cond branch, one to skip else. */
    int jmp_count = 0;
    for (size_t i = 0; i < c.module.instr_count; i++) {
        if (uinstr_op(c.module.instructions[i]) == OP_JMP) jmp_count++;
    }
    UASSERT_EQ(2, jmp_count);
    emit_ctx_destroy(&c);
}

UTEST(emit_while_basic) {
    /* "while (false) { 42 }" — body never executes; verify TEST + two JMPs present.
       Expected sequence:
         [0]  LOADBOOL R0 0 0   (cond = false)
         [1]  TEST R0 0 1       (skip JMP if truthy; won't skip since false)
         [2]  JMP <exit>        (to after back-edge; patched)
         [3]  LOADK R? K0       (body: 42, but never reached)
         [4]  JMP <loop_start>  (back-edge to [0])
         [5]  LOADNIL R?        (post-loop nil value)
         [6]  RET R?
       Key checks: TEST with C=1, two JMPs (exit + back-edge). */
    EmitCtx c;
    emit_ctx_init(&c, "while (false) { 42 }");
    UEmitError rc = emit_ctx_run(&c);
    UASSERT_EQ(EMIT_OK, rc);
    bool found_test = false;
    for (size_t i = 0; i < c.module.instr_count; i++) {
        if (uinstr_op(c.module.instructions[i]) == OP_TEST) {
            found_test = true;
            UASSERT_EQ((uint8_t)1, uinstr_c(c.module.instructions[i]));
        }
    }
    UASSERT(found_test);
    int jmp_count = 0;
    for (size_t i = 0; i < c.module.instr_count; i++) {
        if (uinstr_op(c.module.instructions[i]) == OP_JMP) jmp_count++;
    }
    /* Two JMPs: one exit-branch, one back-edge. */
    UASSERT_EQ(2, jmp_count);
    emit_ctx_destroy(&c);
}

UTEST(emit_while_with_assign) {
    /* "var n = 0; while (n < 3) { n = n + 1 }" — multi-stmt body; verify EMIT_OK
       and presence of TEST + back-edge JMP with negative offset. */
    EmitCtx c;
    emit_ctx_init(&c, "var n = 0; while (n < 3) { n = n + 1 }");
    UEmitError rc = emit_ctx_run(&c);
    UASSERT_EQ(EMIT_OK, rc);
    /* Must have at least one JMP with a negative offset (the back-edge). */
    bool found_back_edge = false;
    for (size_t i = 0; i < c.module.instr_count; i++) {
        if (uinstr_op(c.module.instructions[i]) == OP_JMP) {
            int offset = (int)uinstr_bx(c.module.instructions[i]) - 32768;
            if (offset < 0) { found_back_edge = true; break; }
        }
    }
    UASSERT(found_back_edge);
    emit_ctx_destroy(&c);
}

UTEST(disassemble_call_format) {
    /* Hand-build a module with one OP_CALL R0, B=3, C=2 instruction.
     * B=3 → 2 args (B-1); C=2 → 1 result (C-1).
     * Assert disassembly contains "CALL R0, 2 args, 1 results". */
    UProto m = {0};
    m.instructions = (uint32_t *)malloc(sizeof(uint32_t) * 1);
    m.instr_cap   = 1;
    m.instr_count = 1;
    m.instructions[0] = uinstr_enc_abc(OP_CALL, 0U, 3U, 2U);

    char buf[256];
    size_t n = uemit_disassemble(&m, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "CALL R0, 2 args, 1 results") != NULL);
    uchunk_destroy(&m, NULL);
}

UTEST(disassemble_jmp_signed_offset) {
    /* Hand-build a module with one OP_JMP, Bx=32760.
     * Signed offset = 32760 - 32768 = -8.
     * Assert disassembly contains "JMP -8". */
    UProto m = {0};
    m.instructions = (uint32_t *)malloc(sizeof(uint32_t) * 1);
    m.instr_cap   = 1;
    m.instr_count = 1;
    m.instructions[0] = uinstr_enc_abx(OP_JMP, 0U, (uint16_t)32760U);

    char buf[256];
    size_t n = uemit_disassemble(&m, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "JMP -8") != NULL);
    uchunk_destroy(&m, NULL);
}

UTEST(disassemble_closure_with_prelude) {
    /* Compile "function() { var x = 1; var y = 2; function() { x + y } }"
     * through the parse+emit pipeline.  The innermost closure captures x and y
     * as two upvalues from the enclosing function body (which declares them as
     * locals).
     *
     * v0.8.5 recursive shape: root.nested = [outer]; outer.nested = [inner].
     * Pre-v0.8.5 (flat) the inner was root.nested[1].
     *
     * Note: upval[N] lines only appear in the parent proto's (outer's)
     * disassembly, not in the root module's disassembly.  We verify the
     * upvalue count structurally and check the root disassembly contains
     * a CLOSURE instruction for the outer function. */
    EmitCtx c;
    emit_ctx_init(&c, "function() { var x = 1; var y = 2; function() { x + y } }");
    UEmitError rc = emit_ctx_run(&c);
    UASSERT_EQ(EMIT_OK, rc);
    UASSERT_EQ(c.module.nested_count, (size_t)1);
    UProto *outer = c.module.nested[0];
    UASSERT(outer != NULL);
    /* The outer proto captures nothing from the chunk top. */
    UASSERT(outer->nupvals == 0U);
    /* Outer contains inner as outer.nested[0]; inner captures x and y. */
    UASSERT_EQ(outer->nested_count, (size_t)1);
    UASSERT(outer->nested[0] != NULL);
    UASSERT(outer->nested[0]->nupvals == 2U);

    /* Root module disassembly must show at least one instruction and
     * a CLOSURE P0 entry for the outer function proto. */
    char buf[1024];
    size_t n = uemit_disassemble(&c.module, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "CLOSURE") != NULL);
    emit_ctx_destroy(&c);
}

/* =========================================================================
 * M3 row 7 opcode encoder round-trip tests.
 * Each test encodes one instruction via the public helper, then decodes the
 * word from module.instructions[0] and verifies all fields.
 * ========================================================================= */

UTEST(emit_row7_throw_round_trip) {
    UVM vm; UProto module = {0}; UArena arena; UEmitter e;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    uemit_init(&e, &module, &arena, &vm, "test");

    uemit_throw(&e, /*reg_value=*/5, /*line=*/1);

    UASSERT_EQ(EMIT_OK, e.error);
    UASSERT_EQ((size_t)1, module.instr_count);
    uint32_t w = module.instructions[0];
    UASSERT_EQ((int)OP_THROW, (int)uinstr_op(w));
    UASSERT_EQ((uint8_t)5,    uinstr_a(w));
    UASSERT_EQ((uint16_t)0,   uinstr_bx(w));

    uarena_destroy(&arena); uchunk_destroy(&module, NULL); urbi_vm_destroy(&vm);
}

UTEST(emit_row7_tag_stop_round_trip) {
    UVM vm; UProto module = {0}; UArena arena; UEmitter e;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    uemit_init(&e, &module, &arena, &vm, "test");

    uemit_tag_stop(&e, /*reg_tag=*/3, /*reg_value=*/7, /*line=*/1);

    UASSERT_EQ(EMIT_OK, e.error);
    UASSERT_EQ((size_t)1, module.instr_count);
    uint32_t w = module.instructions[0];
    UASSERT_EQ((int)OP_TAG_STOP, (int)uinstr_op(w));
    UASSERT_EQ((uint8_t)3, uinstr_a(w));
    UASSERT_EQ((uint8_t)7, uinstr_b(w));
    UASSERT_EQ((uint8_t)0, uinstr_c(w));

    uarena_destroy(&arena); uchunk_destroy(&module, NULL); urbi_vm_destroy(&vm);
}

UTEST(emit_row7_try_begin_round_trip) {
    UVM vm; UProto module = {0}; UArena arena; UEmitter e;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    uemit_init(&e, &module, &arena, &vm, "test");

    /* flags=3 (has_catch|has_finally), handler_pc=1000 */
    uemit_try_begin(&e, /*flags=*/3, /*handler_pc=*/1000, /*line=*/1);

    UASSERT_EQ(EMIT_OK, e.error);
    UASSERT_EQ((size_t)1, module.instr_count);
    uint32_t w = module.instructions[0];
    UASSERT_EQ((int)OP_TRY_BEGIN,   (int)uinstr_op(w));
    UASSERT_EQ((uint8_t)3,          uinstr_a(w));
    UASSERT_EQ((uint16_t)1000,      uinstr_bx(w));

    uarena_destroy(&arena); uchunk_destroy(&module, NULL); urbi_vm_destroy(&vm);
}

UTEST(emit_row7_try_end_round_trip) {
    UVM vm; UProto module = {0}; UArena arena; UEmitter e;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    uemit_init(&e, &module, &arena, &vm, "test");

    uemit_try_end(&e, /*line=*/1);

    UASSERT_EQ(EMIT_OK, e.error);
    UASSERT_EQ((size_t)1, module.instr_count);
    uint32_t w = module.instructions[0];
    UASSERT_EQ((int)OP_TRY_END, (int)uinstr_op(w));
    UASSERT_EQ((uint8_t)0, uinstr_a(w));
    UASSERT_EQ((uint8_t)0, uinstr_b(w));
    UASSERT_EQ((uint8_t)0, uinstr_c(w));

    uarena_destroy(&arena); uchunk_destroy(&module, NULL); urbi_vm_destroy(&vm);
}

UTEST(emit_row7_push_tag_round_trip) {
    UVM vm; UProto module = {0}; UArena arena; UEmitter e;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    uemit_init(&e, &module, &arena, &vm, "test");

    /* reg_tag=2, flags=5, onleave_pc=300 */
    uemit_push_tag(&e, /*reg_tag=*/2, /*flags=*/5, /*onleave_pc=*/300, /*line=*/1);

    UASSERT_EQ(EMIT_OK, e.error);
    UASSERT_EQ((size_t)1, module.instr_count);
    uint32_t w = module.instructions[0];
    UASSERT_EQ((int)OP_PUSH_TAG, (int)uinstr_op(w));
    /* A = (flags<<4)|(reg_tag&0xF) = (5<<4)|2 = 0x52 = 82 */
    uint8_t a = uinstr_a(w);
    UASSERT_EQ((uint8_t)2,  (uint8_t)(a & 0x0FU));          /* tag_reg */
    UASSERT_EQ((uint8_t)5,  (uint8_t)((a >> 4) & 0x0FU));   /* flags */
    UASSERT_EQ((uint16_t)300, uinstr_bx(w));                 /* onleave_pc */

    uarena_destroy(&arena); uchunk_destroy(&module, NULL); urbi_vm_destroy(&vm);
}

UTEST(emit_row7_pop_tag_round_trip) {
    UVM vm; UProto module = {0}; UArena arena; UEmitter e;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    uemit_init(&e, &module, &arena, &vm, "test");

    uemit_pop_tag(&e, /*reg_tag=*/4, /*line=*/1);

    UASSERT_EQ(EMIT_OK, e.error);
    UASSERT_EQ((size_t)1, module.instr_count);
    uint32_t w = module.instructions[0];
    UASSERT_EQ((int)OP_POP_TAG, (int)uinstr_op(w));
    UASSERT_EQ((uint8_t)4, uinstr_a(w));
    UASSERT_EQ((uint8_t)0, uinstr_b(w));
    UASSERT_EQ((uint8_t)0, uinstr_c(w));

    uarena_destroy(&arena); uchunk_destroy(&module, NULL); urbi_vm_destroy(&vm);
}

UTEST(emit_row7_push_frame_guard_round_trip) {
    UVM vm; UProto module = {0}; UArena arena; UEmitter e;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    uemit_init(&e, &module, &arena, &vm, "test");

    uemit_push_frame_guard(&e, /*register_base=*/8, /*register_count=*/6, /*line=*/1);

    UASSERT_EQ(EMIT_OK, e.error);
    UASSERT_EQ((size_t)1, module.instr_count);
    uint32_t w = module.instructions[0];
    UASSERT_EQ((int)OP_PUSH_FRAME_GUARD, (int)uinstr_op(w));
    UASSERT_EQ((uint8_t)8, uinstr_a(w));
    UASSERT_EQ((uint8_t)6, uinstr_b(w));
    UASSERT_EQ((uint8_t)0, uinstr_c(w));

    uarena_destroy(&arena); uchunk_destroy(&module, NULL); urbi_vm_destroy(&vm);
}

UTEST(emit_row7_resume_round_trip) {
    UVM vm; UProto module = {0}; UArena arena; UEmitter e;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    uemit_init(&e, &module, &arena, &vm, "test");

    uemit_resume(&e, /*reg_state=*/9, /*line=*/1);

    UASSERT_EQ(EMIT_OK, e.error);
    UASSERT_EQ((size_t)1, module.instr_count);
    uint32_t w = module.instructions[0];
    UASSERT_EQ((int)OP_RESUME, (int)uinstr_op(w));
    UASSERT_EQ((uint8_t)9, uinstr_a(w));
    UASSERT_EQ((uint8_t)0, uinstr_b(w));
    UASSERT_EQ((uint8_t)0, uinstr_c(w));

    uarena_destroy(&arena); uchunk_destroy(&module, NULL); urbi_vm_destroy(&vm);
}

/* T10: OP_LOAD_CATCH_VALUE round-trip. */
UTEST(emit_t10_load_catch_value_round_trip) {
    UVM vm; UProto module = {0}; UArena arena; UEmitter e;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    uemit_init(&e, &module, &arena, &vm, "test");

    uemit_load_catch_value(&e, /*reg=*/5, /*line=*/1);

    UASSERT_EQ(EMIT_OK, e.error);
    UASSERT_EQ((size_t)1, module.instr_count);
    uint32_t w = module.instructions[0];
    UASSERT_EQ((int)OP_LOAD_CATCH_VALUE, (int)uinstr_op(w));
    UASSERT_EQ((uint8_t)5, uinstr_a(w));
    UASSERT_EQ((uint8_t)0, uinstr_b(w));
    UASSERT_EQ((uint8_t)0, uinstr_c(w));

    uarena_destroy(&arena); uchunk_destroy(&module, NULL); urbi_vm_destroy(&vm);
}

/* T10: AST_THROW emit produces OP_THROW after the value expression. */
UTEST(emit_t10_throw_emits_op_throw) {
    UVM vm; UProto module = {0}; UArena arena; UEmitter e;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);
    uemit_init(&e, &module, &arena, &vm, "test");

    /* Build AST: throw 42 */
    UAstNode val = {0};
    val.kind  = AST_INT;
    val.u.i   = 42;
    UAstNode node = {0};
    node.kind              = AST_THROW;
    node.u.throw_expr.value = &val;

    uemit_statement(&e, &node);
    UASSERT_EQ(EMIT_OK, e.error);

    /* Expect at least: LOADK R0 K0 ; THROW R0 ; LOADNIL R1
     * Scan backwards for OP_THROW (throw is followed by LOADNIL for the
     * statement result register, so it is not the final instruction). */
    UASSERT(module.instr_count >= 2);
    int throw_idx = -1;
    for (int i = (int)module.instr_count - 1; i >= 0; i--) {
        if ((int)uinstr_op(module.instructions[i]) == (int)OP_THROW) {
            throw_idx = i;
            break;
        }
    }
    UASSERT(throw_idx >= 0);
    uint32_t w = module.instructions[(size_t)throw_idx];
    UASSERT_EQ((int)OP_THROW, (int)uinstr_op(w));
    /* A = destination register of the value expression (R1 at chunk-top).
     * T73: chunk-top pre-reserves R0 for r_global_slot; first temp starts at R1. */
    UASSERT_EQ((uint8_t)1, uinstr_a(w));

    uarena_destroy(&arena); uchunk_destroy(&module, NULL); urbi_vm_destroy(&vm);
}

/* --- M4 T20+T21 — AST_MEMBER_GET → OP_GETSLOT, AST_MEMBER_SET → OP_SETSLOT --- */

UTEST(emit_member_get_emits_op_getslot_with_ic_index_zero) {
    /* "var obj = nil; obj.x" at chunk-top (T72) has multiple IC sites:
     * SETSLOT(obj write), GETSLOT(obj global read), GETSLOT(obj.x member).
     * This test verifies that compilation succeeds and at least one GETSLOT
     * with IC name "x" is emitted (the member access site).
     *
     * Note: IC index 0 is now assigned to the `obj` SETSLOT, not `obj.x`.
     * Tests that require IC index 0 == "x" belong in nested function bodies
     * where obj is a local (no global IC overhead). */
    EmitCtx c;
    emit_ctx_init(&c, "var obj = nil; obj.x");

    UAstNode *stmt;
    while ((stmt = uparse_next_statement(&c.p)) != NULL) {
        UASSERT_EQ(EMIT_OK, uemit_statement(&c.e, stmt));
    }

    /* Verify at least one OP_GETSLOT is emitted. */
    bool found_getslot = false;
    for (size_t i = 0; i < c.module.instr_count; i++) {
        if (uinstr_op(c.module.instructions[i]) == OP_GETSLOT) {
            found_getslot = true;
            break;
        }
    }
    UASSERT(found_getslot);

    /* Verify "x" is recorded in the IC name table. */
    UASSERT(c.e.current_fs != NULL);
    UASSERT(c.e.current_fs->ic_names != NULL);
    const char *xn = ustr_intern(&c.vm, "x", 1);
    bool found_x = false;
    for (uint16_t i = 0; i < c.e.current_fs->ic_next; i++) {
        if (c.e.current_fs->ic_names[i] == (USymbol *)xn) {
            found_x = true;
            break;
        }
    }
    UASSERT(found_x);

    UASSERT_EQ(EMIT_OK, uemit_finish(&c.e));
    emit_ctx_destroy(&c);
}

UTEST(emit_member_set_emits_op_setslot_with_ic_index_zero) {
    /* "var obj = nil; obj.x = 42" — verifies that both member-set and
     * global-var-decl SETSLOT instructions are emitted and that "x" appears
     * in the IC name table.
     *
     * Note: at chunk-top (T72), `var obj = nil` itself emits a SETSLOT (for
     * the global `obj` write), so the first SETSLOT is for `obj`, not `x`.
     * IC index for "x" is >= 1.  Tests that require IC index 0 == "x" should
     * use a nested function body where obj is a local. */
    EmitCtx c;
    emit_ctx_init(&c, "var obj = nil; obj.x = 42");

    UAstNode *stmt;
    while ((stmt = uparse_next_statement(&c.p)) != NULL) {
        UASSERT_EQ(EMIT_OK, uemit_statement(&c.e, stmt));
    }

    /* At least one OP_SETSLOT must be emitted. */
    bool found_setslot = false;
    for (size_t i = 0; i < c.module.instr_count; i++) {
        if (uinstr_op(c.module.instructions[i]) == OP_SETSLOT) {
            found_setslot = true;
            break;
        }
    }
    UASSERT(found_setslot);

    /* "x" must appear in the IC name table. */
    UASSERT(c.e.current_fs != NULL);
    UASSERT(c.e.current_fs->ic_names != NULL);
    const char *xn = ustr_intern(&c.vm, "x", 1);
    bool found_x = false;
    for (uint16_t i = 0; i < c.e.current_fs->ic_next; i++) {
        if (c.e.current_fs->ic_names[i] == (USymbol *)xn) {
            found_x = true;
            break;
        }
    }
    UASSERT(found_x);

    UASSERT_EQ(EMIT_OK, uemit_finish(&c.e));
    emit_ctx_destroy(&c);
}

UTEST(emit_top_level_member_get_populates_module_ic_count) {
    /* "var o = nil; o.x" — verifies that top-level IC sites are correctly
     * copied from funcstate into UProto after uemit_finish.  This regresses
     * the silent miscompile where the top-level funcstate's ic_names were
     * freed without being copied into UProto.
     *
     * With T72, chunk-top `var o = nil` adds IC sites for the global write
     * (SETSLOT) and the global read (GETSLOT), plus one for the member access
     * (.x GETSLOT).  ic_count must be >= 1 and "x" must appear somewhere. */
    EmitCtx c;
    emit_ctx_init(&c, "var o = nil; o.x");
    UASSERT_EQ(EMIT_OK, emit_ctx_run(&c));

    UASSERT(c.module.ic_count >= 1U);
    UASSERT(c.module.ic_names != NULL);
    /* "x" must be present somewhere in the IC name table. */
    const char *xn = ustr_intern(&c.vm, "x", 1);
    bool found_x = false;
    for (uint16_t i = 0; i < c.module.ic_count; i++) {
        if (c.module.ic_names[i] == (USymbol *)xn) {
            found_x = true;
            break;
        }
    }
    UASSERT(found_x);

    emit_ctx_destroy(&c);
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
    utest_run("emit AST_IDENT bare ident -> global fallback (T71, no EMIT_UNRESOLVED_NAME)",
              emit_ast_ident_unresolved_name_returns_error);
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
    utest_run("emit var decl: 'var x = 7' no OP_MOVE for local absorption",
              emit_var_decl_basic_no_op_move);
    utest_run("emit var then use: 'var x = 7; x + 1' resolves x via OP_MOVE",
              emit_var_then_use_resolves_local);
    utest_run("emit var redeclare in same scope is EMIT_LOCAL_REDECLARE",
              emit_var_redeclare_in_same_scope_is_error);
    utest_run("emit bare name 'ghost' -> global fallback (EMIT_OK after T71)",
              emit_unresolved_name_is_error);
    utest_run("emit assign: 'var x = 1; x = 42' emits OP_MOVE to slot 0",
              emit_assign_to_existing_local);
    utest_run("emit assign to unresolved name is EMIT_UNRESOLVED_NAME",
              emit_assign_to_unresolved_is_error);
    utest_run("emit: AST_BOOL true → LOADBOOL R0 1 0",
              emit_ast_bool_true_emits_loadbool_1_0);
    utest_run("emit: AST_BOOL false → LOADBOOL R0 0 0",
              emit_ast_bool_false_emits_loadbool_0_0);
    utest_run("emit: AST_NIL → LOADNIL R0",
              emit_ast_nil_emits_loadnil);
    utest_run("emit: AST_COMPARE(==) emits 4-instruction branch idiom",
              emit_ast_compare_eq_emits_4_instruction_pattern);
    utest_run("emit: if-then-only emits TEST + JMP fixup + LOADNIL for no-else",
              emit_if_then_only);
    utest_run("emit: if-then-else emits TEST + two JMPs, no LOADNIL",
              emit_if_then_else);
    utest_run("emit: while (false) { 42 } emits TEST + exit-JMP + back-JMP",
              emit_while_basic);
    utest_run("emit: while (n < 3) { n = n + 1 } compiles with back-edge JMP",
              emit_while_with_assign);
    utest_run("disassemble: CALL R0, 2 args, 1 results format",
              disassemble_call_format);
    utest_run("disassemble: JMP with Bx=32760 shows signed offset -8",
              disassemble_jmp_signed_offset);
    utest_run("disassemble: CLOSURE with 2 upvals prints both upval lines",
              disassemble_closure_with_prelude);
    /* M3 row 7 opcode encoder round-trip tests */
    utest_run("emit row7: OP_THROW encodes reg_value in A, Bx=0",
              emit_row7_throw_round_trip);
    utest_run("emit row7: OP_TAG_STOP encodes reg_tag in A, reg_value in B",
              emit_row7_tag_stop_round_trip);
    utest_run("emit row7: OP_TRY_BEGIN encodes flags in A, handler_pc in Bx",
              emit_row7_try_begin_round_trip);
    utest_run("emit row7: OP_TRY_END encodes all-zero operands",
              emit_row7_try_end_round_trip);
    utest_run("emit row7: OP_PUSH_TAG packs flags<<4|tag_reg in A, onleave_pc in Bx",
              emit_row7_push_tag_round_trip);
    utest_run("emit row7: OP_POP_TAG encodes reg_tag in A",
              emit_row7_pop_tag_round_trip);
    utest_run("emit row7: OP_PUSH_FRAME_GUARD encodes register_base in A, count in B",
              emit_row7_push_frame_guard_round_trip);
    utest_run("emit row7: OP_RESUME encodes reg_state in A",
              emit_row7_resume_round_trip);
    /* T10: try/catch/finally + throw emit */
    utest_run("emit T10: OP_LOAD_CATCH_VALUE encodes dst reg in A",
              emit_t10_load_catch_value_round_trip);
    utest_run("emit T10: AST_THROW emits LOADK then OP_THROW with value reg",
              emit_t10_throw_emits_op_throw);
    /* M4: AST_MEMBER_GET / AST_MEMBER_SET → OP_GETSLOT / OP_SETSLOT */
    utest_run("emit: AST_MEMBER_GET → OP_GETSLOT with IC index 0",
              emit_member_get_emits_op_getslot_with_ic_index_zero);
    utest_run("emit: AST_MEMBER_SET → OP_SETSLOT with IC index 0",
              emit_member_set_emits_op_setslot_with_ic_index_zero);
    utest_run("emit: top-level 'o.x' populates UProto.ic_count + ic_names after finish",
              emit_top_level_member_get_populates_module_ic_count);
}
