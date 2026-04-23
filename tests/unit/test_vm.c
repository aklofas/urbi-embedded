/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include "uvm.h"
#include "uchunk.h"
#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Sentinel allocator for testing that uvm_init preserves an explicit
   alloc_fn. Real function address, so the test is ISO-C-clean (no
   object-to-function-pointer cast) and genuinely proves the stdlib
   shim did not overwrite the caller's choice. */
static void *noop_alloc(void *ptr, size_t nbytes, void *ud) {
    (void)ptr; (void)nbytes; (void)ud;
    return NULL;
}

UTEST(vm_error_name_covers_all_codes) {
    UASSERT_EQ(0, strcmp("UVM_OK",         uvm_error_name(UVM_OK)));
    UASSERT_EQ(0, strcmp("UVM_TYPE_ERROR", uvm_error_name(UVM_TYPE_ERROR)));
    UASSERT_EQ(0, strcmp("UVM_OOM",        uvm_error_name(UVM_OOM)));
}

UTEST(vm_init_hosted_null_alloc_falls_back_to_stdlib) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    UASSERT(vm.alloc_fn != NULL);  /* stdlib shim installed */
    UASSERT_EQ(UVM_OK, vm.last_error);
    UASSERT_EQ('\0', vm.last_errmsg[0]);
    uvm_destroy(&vm);
}

UTEST(vm_init_with_explicit_alloc_preserves_it) {
    UVM vm;
    void *sentinel_ud = (void *)&vm;  /* any non-NULL object pointer works */
    uvm_init(&vm, noop_alloc, sentinel_ud);
    UASSERT(vm.alloc_fn == noop_alloc);
    UASSERT(vm.alloc_ud == sentinel_ud);
    uvm_destroy(&vm);
}

UTEST(vm_init_zeroes_last_error_and_msg) {
    UVM vm;
    /* Pre-dirty the struct so we can tell init zeroed it. */
    memset(&vm, 0xAA, sizeof(vm));
    uvm_init(&vm, NULL, NULL);
    UASSERT_EQ(UVM_OK, vm.last_error);
    UASSERT_EQ('\0', vm.last_errmsg[0]);
    uvm_destroy(&vm);
}

UTEST(vm_destroy_on_zero_initialized_is_safe) {
    UVM vm = {0};
    uvm_destroy(&vm);  /* must not crash, deref, or free garbage */
    UASSERT(1);  /* reached this line */
}

UTEST(vm_destroy_twice_is_safe) {
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    uvm_destroy(&vm);
    uvm_destroy(&vm);  /* idempotent */
    UASSERT(1);
}

/* --- uvm_run empty-chunk + OP_RET --- */

/* Build a tiny chunk by direct struct init — bypasses the serialize
   round-trip. This is fine for VM unit tests; test_chunk.c already
   validates the loader path. All these fabricated chunks use stdlib
   for allocation so uchunk_destroy can free them uniformly. */

static void fab_chunk_empty(Chunk *c) {
    memset(c, 0, sizeof(*c));
    c->max_reg = 0;
    /* No instructions, no constants, no synclines. Loader accepts this. */
}

static void fab_chunk_ret_only(Chunk *c, uint8_t reg) {
    memset(c, 0, sizeof(*c));
    c->max_reg = reg;
    c->instructions = (uint32_t *)malloc(sizeof(uint32_t) * 1);
    c->instr_cap = 1;
    c->instr_count = 1;
    c->instructions[0] = uinstr_enc_abc(OP_RET, reg, 0, 0);
    c->line_deltas = (int8_t *)malloc(sizeof(int8_t) * 1);
    c->line_deltas[0] = 1;  /* pc 0 at line 1 */
}

UTEST(vm_run_empty_chunk_returns_nil) {
    Chunk c; fab_chunk_empty(&c);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UConst out;
    memset(&out, 0xAA, sizeof(out));  /* pre-dirty to confirm VM writes it */
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_NIL, out.kind);
    uvm_destroy(&vm);
    /* c has no owned buffers; no destroy needed */
}

UTEST(vm_run_ret_on_uninitialized_register_returns_nil) {
    /* Frame is zero-initialized to UVAL_NIL, so RET R[0] on a chunk with
       no LOADK returns Nil. */
    Chunk c; fab_chunk_ret_only(&c, 0);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UConst out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_NIL, out.kind);
    free(c.instructions);
    free(c.line_deltas);
    uvm_destroy(&vm);
}

/* Build a chunk with one LOADK A=0 Bx=0 then RET R[0]. The constant is
   Integer `value`. */
static void fab_chunk_loadk_int_ret(Chunk *c, int64_t value) {
    memset(c, 0, sizeof(*c));
    c->max_reg = 0;
    c->instructions = (uint32_t *)malloc(sizeof(uint32_t) * 2);
    c->instr_cap = 2;
    c->instr_count = 2;
    c->instructions[0] = uinstr_enc_abx(OP_LOADK, 0, 0);
    c->instructions[1] = uinstr_enc_abc(OP_RET, 0, 0, 0);
    c->constants = (UConst *)malloc(sizeof(UConst) * 1);
    c->const_cap = 1;
    c->const_count = 1;
    c->constants[0].kind = UVAL_INT;
    c->constants[0].v.i  = value;
    c->line_deltas = (int8_t *)malloc(sizeof(int8_t) * 2);
    c->line_deltas[0] = 1;
    c->line_deltas[1] = 0;
}

static void fab_chunk_loadk_float_ret(Chunk *c, double value) {
    fab_chunk_loadk_int_ret(c, 0);  /* shape is identical */
    c->constants[0].kind = UVAL_FLOAT;
    c->constants[0].v.f = (URBI_FLOAT_TYPE == 8) ? value : (float)value;
}

static void free_fab_chunk(Chunk *c) {
    free(c->instructions);
    free(c->constants);
    free(c->line_deltas);
    free(c->abs_lines);
}

UTEST(vm_loadk_int) {
    Chunk c; fab_chunk_loadk_int_ret(&c, 42);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UConst out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(42, out.v.i);
    free_fab_chunk(&c);
    uvm_destroy(&vm);
}

UTEST(vm_loadk_int_large) {
    Chunk c; fab_chunk_loadk_int_ret(&c, INT64_MAX);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UConst out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT(out.v.i == INT64_MAX);
    free_fab_chunk(&c);
    uvm_destroy(&vm);
}

UTEST(vm_loadk_float) {
    Chunk c; fab_chunk_loadk_float_ret(&c, 3.14);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UConst out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    UASSERT(out.v.f > 3.13 && out.v.f < 3.15);
    free_fab_chunk(&c);
    uvm_destroy(&vm);
}

/* --- OP_ADD --- */

/* Build LOADK R[0]=a, LOADK R[1]=b, ADD R[2]=R[0]+R[1], RET R[2]. */
static void fab_chunk_int_add_int(Chunk *c, int64_t a, int64_t b) {
    memset(c, 0, sizeof(*c));
    c->max_reg = 2;
    c->instructions = (uint32_t *)malloc(sizeof(uint32_t) * 4);
    c->instr_cap = 4; c->instr_count = 4;
    c->instructions[0] = uinstr_enc_abx(OP_LOADK, 0, 0);
    c->instructions[1] = uinstr_enc_abx(OP_LOADK, 1, 1);
    c->instructions[2] = uinstr_enc_abc(OP_ADD, 2, 0, 1);
    c->instructions[3] = uinstr_enc_abc(OP_RET, 2, 0, 0);
    c->constants = (UConst *)malloc(sizeof(UConst) * 2);
    c->const_cap = 2; c->const_count = 2;
    c->constants[0].kind = UVAL_INT; c->constants[0].v.i = a;
    c->constants[1].kind = UVAL_INT; c->constants[1].v.i = b;
    c->line_deltas = (int8_t *)malloc(sizeof(int8_t) * 4);
    c->line_deltas[0] = 1;
    c->line_deltas[1] = 0;
    c->line_deltas[2] = 0;
    c->line_deltas[3] = 0;
}

UTEST(vm_add_int_int_normal) {
    Chunk c; fab_chunk_int_add_int(&c, 2, 3);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UConst out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(5, out.v.i);
    free_fab_chunk(&c); uvm_destroy(&vm);
}

UTEST(vm_add_int_int_max_plus_one_wraps) {
    Chunk c; fab_chunk_int_add_int(&c, INT64_MAX, 1);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UConst out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT(out.v.i == INT64_MIN);
    free_fab_chunk(&c); uvm_destroy(&vm);
}

UTEST(vm_add_int_int_max_plus_max_wraps_to_minus_two) {
    Chunk c; fab_chunk_int_add_int(&c, INT64_MAX, INT64_MAX);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UConst out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT(out.v.i == -2);
    free_fab_chunk(&c); uvm_destroy(&vm);
}

/* ADD R[2] = R[0] + R[1] with mixed Int/Float constants. Accepts both
   Int and Float literals via the UConst `kind` parameter. */
static void fab_chunk_add_mixed(Chunk *c,
                                UValKind kind_a, int64_t ai, double af,
                                UValKind kind_b, int64_t bi, double bf) {
    fab_chunk_int_add_int(c, 0, 0);  /* shape is identical */
    c->constants[0].kind = kind_a;
    if (kind_a == UVAL_INT) c->constants[0].v.i = ai;
    else c->constants[0].v.f = (URBI_FLOAT_TYPE == 8) ? af : (float)af;
    c->constants[1].kind = kind_b;
    if (kind_b == UVAL_INT) c->constants[1].v.i = bi;
    else c->constants[1].v.f = (URBI_FLOAT_TYPE == 8) ? bf : (float)bf;
}

UTEST(vm_add_int_float_promotes) {
    Chunk c; fab_chunk_add_mixed(&c, UVAL_INT, 2, 0, UVAL_FLOAT, 0, 1.5);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UConst out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    UASSERT(out.v.f > 3.49 && out.v.f < 3.51);
    free_fab_chunk(&c); uvm_destroy(&vm);
}

UTEST(vm_add_float_int_promotes) {
    Chunk c; fab_chunk_add_mixed(&c, UVAL_FLOAT, 0, 2.5, UVAL_INT, 3, 0);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UConst out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    UASSERT(out.v.f > 5.49 && out.v.f < 5.51);
    free_fab_chunk(&c); uvm_destroy(&vm);
}

UTEST(vm_add_float_float) {
    Chunk c; fab_chunk_add_mixed(&c, UVAL_FLOAT, 0, 1.25, UVAL_FLOAT, 0, 2.75);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UConst out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    UASSERT(out.v.f > 3.99 && out.v.f < 4.01);
    free_fab_chunk(&c); uvm_destroy(&vm);
}

UTEST(vm_add_bool_int_is_type_error) {
    Chunk c; fab_chunk_add_mixed(&c, UVAL_BOOL, 1, 0, UVAL_INT, 5, 0);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UConst out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_NIL, out.kind);
    free_fab_chunk(&c); uvm_destroy(&vm);
}

UTEST(vm_add_int_nil_is_type_error) {
    Chunk c; fab_chunk_add_mixed(&c, UVAL_INT, 5, 0, UVAL_NIL, 0, 0);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UConst out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_NIL, out.kind);
    free_fab_chunk(&c); uvm_destroy(&vm);
}

/* --- OP_MOVE --- */

/* Build LOADK R[0]=value; MOVE R[1]=R[0]; RET R[1]. */
static void fab_chunk_loadk_move_ret(Chunk *c, int64_t value) {
    memset(c, 0, sizeof(*c));
    c->max_reg = 1;
    c->instructions = (uint32_t *)malloc(sizeof(uint32_t) * 3);
    c->instr_cap = 3;
    c->instr_count = 3;
    c->instructions[0] = uinstr_enc_abx(OP_LOADK, 0, 0);
    c->instructions[1] = uinstr_enc_abc(OP_MOVE, 1, 0, 0);
    c->instructions[2] = uinstr_enc_abc(OP_RET, 1, 0, 0);
    c->constants = (UConst *)malloc(sizeof(UConst) * 1);
    c->const_cap = 1;
    c->const_count = 1;
    c->constants[0].kind = UVAL_INT;
    c->constants[0].v.i  = value;
    c->line_deltas = (int8_t *)malloc(sizeof(int8_t) * 3);
    c->line_deltas[0] = 1;
    c->line_deltas[1] = 0;
    c->line_deltas[2] = 0;
}

UTEST(vm_move_copies_register) {
    Chunk c; fab_chunk_loadk_move_ret(&c, 99);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UConst out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(99, out.v.i);
    free_fab_chunk(&c);
    uvm_destroy(&vm);
}

UTEST(vm_move_self_copy_is_noop) {
    /* LOADK R[0]=7; MOVE R[0]=R[0]; RET R[0]. */
    Chunk c;
    memset(&c, 0, sizeof(c));
    c.max_reg = 0;
    c.instructions = (uint32_t *)malloc(sizeof(uint32_t) * 3);
    c.instr_cap = 3; c.instr_count = 3;
    c.instructions[0] = uinstr_enc_abx(OP_LOADK, 0, 0);
    c.instructions[1] = uinstr_enc_abc(OP_MOVE, 0, 0, 0);
    c.instructions[2] = uinstr_enc_abc(OP_RET, 0, 0, 0);
    c.constants = (UConst *)malloc(sizeof(UConst));
    c.const_cap = 1; c.const_count = 1;
    c.constants[0].kind = UVAL_INT; c.constants[0].v.i = 7;
    c.line_deltas = (int8_t *)malloc(sizeof(int8_t) * 3);
    c.line_deltas[0] = 1; c.line_deltas[1] = 0; c.line_deltas[2] = 0;

    UVM vm; uvm_init(&vm, NULL, NULL);
    UConst out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(7, out.v.i);
    free_fab_chunk(&c);
    uvm_destroy(&vm);
}

void test_vm_suite(void) {
    utest_run("vm_error_name covers all codes", vm_error_name_covers_all_codes);
    utest_run("uvm_init hosted NULL alloc falls back to stdlib shim",
              vm_init_hosted_null_alloc_falls_back_to_stdlib);
    utest_run("uvm_init with explicit alloc preserves it",
              vm_init_with_explicit_alloc_preserves_it);
    utest_run("uvm_init zeroes last_error and last_errmsg",
              vm_init_zeroes_last_error_and_msg);
    utest_run("uvm_destroy on zero-initialized UVM is safe",
              vm_destroy_on_zero_initialized_is_safe);
    utest_run("uvm_destroy twice is safe", vm_destroy_twice_is_safe);
    utest_run("uvm_run on empty chunk returns Nil",
              vm_run_empty_chunk_returns_nil);
    utest_run("uvm_run RET on uninitialized register returns Nil",
              vm_run_ret_on_uninitialized_register_returns_nil);
    utest_run("uvm OP_LOADK Integer into register", vm_loadk_int);
    utest_run("uvm OP_LOADK Integer INT64_MAX preserves value", vm_loadk_int_large);
    utest_run("uvm OP_LOADK Float into register", vm_loadk_float);
    utest_run("uvm OP_MOVE copies register", vm_move_copies_register);
    utest_run("uvm OP_MOVE self-copy is no-op", vm_move_self_copy_is_noop);
    utest_run("uvm OP_ADD Int+Int normal", vm_add_int_int_normal);
    utest_run("uvm OP_ADD Int+Int INT64_MAX+1 wraps to INT64_MIN",
              vm_add_int_int_max_plus_one_wraps);
    utest_run("uvm OP_ADD Int+Int MAX+MAX wraps to -2",
              vm_add_int_int_max_plus_max_wraps_to_minus_two);
    utest_run("uvm OP_ADD Int+Float promotes to Float",
              vm_add_int_float_promotes);
    utest_run("uvm OP_ADD Float+Int promotes to Float",
              vm_add_float_int_promotes);
    utest_run("uvm OP_ADD Float+Float stays Float", vm_add_float_float);
    utest_run("uvm OP_ADD Bool+Int raises TypeError",
              vm_add_bool_int_is_type_error);
    utest_run("uvm OP_ADD Int+Nil raises TypeError",
              vm_add_int_nil_is_type_error);
}
