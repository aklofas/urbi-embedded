/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include "uvm.h"
#include "umodule.h"
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

/* --- uvm_run empty-module + OP_RET --- */

/* Build a tiny module by direct struct init — bypasses the serialize
   round-trip. This is fine for VM unit tests; test_module.c already
   validates the loader path. All these fabricated modules use stdlib
   for allocation so umodule_destroy can free them uniformly. */

static void fab_module_empty(UModule *c) {
    memset(c, 0, sizeof(*c));
    c->max_reg = 0;
    /* No instructions, no constants, no synclines. Loader accepts this. */
}

static void fab_module_ret_only(UModule *c, uint8_t reg) {
    memset(c, 0, sizeof(*c));
    c->max_reg = reg;
    c->instructions = (uint32_t *)malloc(sizeof(uint32_t) * 1);
    c->instr_cap = 1;
    c->instr_count = 1;
    c->instructions[0] = uinstr_enc_abc(OP_RET, reg, 0, 0);
    c->line_deltas = (int8_t *)malloc(sizeof(int8_t) * 1);
    c->line_deltas[0] = 1;  /* pc 0 at line 1 */
}

UTEST(vm_run_empty_module_returns_nil) {
    UModule c; fab_module_empty(&c);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    memset(&out, 0xAA, sizeof(out));  /* pre-dirty to confirm VM writes it */
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_NIL, out.kind);
    uvm_destroy(&vm);
    /* c has no owned buffers; no destroy needed */
}

UTEST(vm_run_ret_on_uninitialized_register_returns_nil) {
    /* Frame is zero-initialized to UVAL_NIL, so RET R[0] on a module with
       no LOADK returns Nil. */
    UModule c; fab_module_ret_only(&c, 0);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_NIL, out.kind);
    free(c.instructions);
    free(c.line_deltas);
    uvm_destroy(&vm);
}

/* Build a module with one LOADK A=0 Bx=0 then RET R[0]. The constant is
   Integer `value`. */
static void fab_module_loadk_int_ret(UModule *c, int64_t value) {
    memset(c, 0, sizeof(*c));
    c->max_reg = 0;
    c->instructions = (uint32_t *)malloc(sizeof(uint32_t) * 2);
    c->instr_cap = 2;
    c->instr_count = 2;
    c->instructions[0] = uinstr_enc_abx(OP_LOADK, 0, 0);
    c->instructions[1] = uinstr_enc_abc(OP_RET, 0, 0, 0);
    c->constants = (UValue *)malloc(sizeof(UValue) * 1);
    c->const_cap = 1;
    c->const_count = 1;
    c->constants[0].kind = UVAL_INT;
    c->constants[0].v.i  = value;
    c->line_deltas = (int8_t *)malloc(sizeof(int8_t) * 2);
    c->line_deltas[0] = 1;
    c->line_deltas[1] = 0;
}

static void fab_module_loadk_float_ret(UModule *c, double value) {
    fab_module_loadk_int_ret(c, 0);  /* shape is identical */
    c->constants[0].kind = UVAL_FLOAT;
    c->constants[0].v.f = (URBI_FLOAT_TYPE == 8) ? value : (float)value;
}

static void free_fab_module(UModule *c) {
    free(c->instructions);
    free(c->constants);
    free(c->line_deltas);
    free(c->abs_lines);
}

UTEST(vm_loadk_int) {
    UModule c; fab_module_loadk_int_ret(&c, 42);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(42, out.v.i);
    free_fab_module(&c);
    uvm_destroy(&vm);
}

UTEST(vm_loadk_int_large) {
    UModule c; fab_module_loadk_int_ret(&c, INT64_MAX);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT(out.v.i == INT64_MAX);
    free_fab_module(&c);
    uvm_destroy(&vm);
}

UTEST(vm_loadk_float) {
    UModule c; fab_module_loadk_float_ret(&c, 3.14);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    UASSERT(out.v.f > 3.13 && out.v.f < 3.15);
    free_fab_module(&c);
    uvm_destroy(&vm);
}

/* --- OP_ADD --- */

/* Build LOADK R[0]=a, LOADK R[1]=b, ADD R[2]=R[0]+R[1], RET R[2]. */
static void fab_module_int_add_int(UModule *c, int64_t a, int64_t b) {
    memset(c, 0, sizeof(*c));
    c->max_reg = 2;
    c->instructions = (uint32_t *)malloc(sizeof(uint32_t) * 4);
    c->instr_cap = 4; c->instr_count = 4;
    c->instructions[0] = uinstr_enc_abx(OP_LOADK, 0, 0);
    c->instructions[1] = uinstr_enc_abx(OP_LOADK, 1, 1);
    c->instructions[2] = uinstr_enc_abc(OP_ADD, 2, 0, 1);
    c->instructions[3] = uinstr_enc_abc(OP_RET, 2, 0, 0);
    c->constants = (UValue *)malloc(sizeof(UValue) * 2);
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
    UModule c; fab_module_int_add_int(&c, 2, 3);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(5, out.v.i);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_add_int_int_max_plus_one_wraps) {
    UModule c; fab_module_int_add_int(&c, INT64_MAX, 1);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT(out.v.i == INT64_MIN);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_add_int_int_max_plus_max_wraps_to_minus_two) {
    UModule c; fab_module_int_add_int(&c, INT64_MAX, INT64_MAX);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT(out.v.i == -2);
    free_fab_module(&c); uvm_destroy(&vm);
}

/* ADD R[2] = R[0] + R[1] with mixed Int/Float constants. Accepts both
   Int and Float literals via the UValue `kind` parameter. */
static void fab_module_add_mixed(UModule *c,
                                UValKind kind_a, int64_t ai, double af,
                                UValKind kind_b, int64_t bi, double bf) {
    fab_module_int_add_int(c, 0, 0);  /* shape is identical */
    c->constants[0].kind = kind_a;
    if (kind_a == UVAL_INT) c->constants[0].v.i = ai;
    else c->constants[0].v.f = (URBI_FLOAT_TYPE == 8) ? af : (float)af;
    c->constants[1].kind = kind_b;
    if (kind_b == UVAL_INT) c->constants[1].v.i = bi;
    else c->constants[1].v.f = (URBI_FLOAT_TYPE == 8) ? bf : (float)bf;
}

UTEST(vm_add_int_float_promotes) {
    UModule c; fab_module_add_mixed(&c, UVAL_INT, 2, 0, UVAL_FLOAT, 0, 1.5);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    UASSERT(out.v.f > 3.49 && out.v.f < 3.51);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_add_float_int_promotes) {
    UModule c; fab_module_add_mixed(&c, UVAL_FLOAT, 0, 2.5, UVAL_INT, 3, 0);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    UASSERT(out.v.f > 5.49 && out.v.f < 5.51);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_add_float_float) {
    UModule c; fab_module_add_mixed(&c, UVAL_FLOAT, 0, 1.25, UVAL_FLOAT, 0, 2.75);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    UASSERT(out.v.f > 3.99 && out.v.f < 4.01);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_add_bool_int_is_type_error) {
    UModule c; fab_module_add_mixed(&c, UVAL_BOOL, 1, 0, UVAL_INT, 5, 0);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_NIL, out.kind);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_add_int_nil_is_type_error) {
    UModule c; fab_module_add_mixed(&c, UVAL_INT, 5, 0, UVAL_NIL, 0, 0);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_NIL, out.kind);
    free_fab_module(&c); uvm_destroy(&vm);
}

/* --- OP_SUB --- */

/* Build an opcode of form R[2] = R[0] OP R[1]; RET R[2]. Parameterized
   over opcode so we can reuse for SUB / MUL / DIV. */
static void fab_module_binop_int(UModule *c, UOpcode op, int64_t a, int64_t b) {
    fab_module_int_add_int(c, a, b);
    c->instructions[2] = uinstr_enc_abc(op, 2, 0, 1);
}

UTEST(vm_sub_int_int_normal) {
    UModule c; fab_module_binop_int(&c, OP_SUB, 5, 3);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(2, out.v.i);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_sub_int_int_min_minus_one_wraps) {
    UModule c; fab_module_binop_int(&c, OP_SUB, INT64_MIN, 1);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT(out.v.i == INT64_MAX);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_sub_float_float) {
    UModule c; fab_module_add_mixed(&c, UVAL_FLOAT, 0, 5.5, UVAL_FLOAT, 0, 1.25);
    c.instructions[2] = uinstr_enc_abc(OP_SUB, 2, 0, 1);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    UASSERT(out.v.f > 4.24 && out.v.f < 4.26);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_sub_bool_operand_is_type_error) {
    UModule c; fab_module_add_mixed(&c, UVAL_INT, 5, 0, UVAL_BOOL, 1, 0);
    c.instructions[2] = uinstr_enc_abc(OP_SUB, 2, 0, 1);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c, &out));
    free_fab_module(&c); uvm_destroy(&vm);
}

/* --- OP_MUL --- */

UTEST(vm_mul_int_int_normal) {
    UModule c; fab_module_binop_int(&c, OP_MUL, 6, 7);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(42, out.v.i);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_mul_int_int_min_times_neg_one_wraps_to_min) {
    UModule c; fab_module_binop_int(&c, OP_MUL, INT64_MIN, -1);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT(out.v.i == INT64_MIN);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_mul_float_int_promotes) {
    UModule c; fab_module_add_mixed(&c, UVAL_FLOAT, 0, 1.5, UVAL_INT, 4, 0);
    c.instructions[2] = uinstr_enc_abc(OP_MUL, 2, 0, 1);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    UASSERT(out.v.f > 5.99 && out.v.f < 6.01);
    free_fab_module(&c); uvm_destroy(&vm);
}

/* --- OP_DIV --- */

UTEST(vm_div_int_int_always_float) {
    UModule c; fab_module_binop_int(&c, OP_DIV, 5, 2);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    UASSERT(out.v.f > 2.49 && out.v.f < 2.51);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_div_int_int_exact_still_float) {
    UModule c; fab_module_binop_int(&c, OP_DIV, 10, 2);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    UASSERT(out.v.f > 4.99 && out.v.f < 5.01);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_div_by_zero_positive_is_inf) {
    UModule c; fab_module_binop_int(&c, OP_DIV, 5, 0);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    /* +Inf is greater than any finite float. */
    UASSERT(out.v.f > 1e30);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_div_by_zero_negative_is_neg_inf) {
    UModule c; fab_module_binop_int(&c, OP_DIV, -5, 0);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    UASSERT(out.v.f < -1e30);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_div_zero_by_zero_is_nan) {
    UModule c; fab_module_binop_int(&c, OP_DIV, 0, 0);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    /* NaN is the only float that compares unequal to itself. */
    UASSERT(out.v.f != out.v.f);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_div_float_float) {
    UModule c; fab_module_add_mixed(&c, UVAL_FLOAT, 0, 7.5, UVAL_FLOAT, 0, 2.5);
    c.instructions[2] = uinstr_enc_abc(OP_DIV, 2, 0, 1);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    UASSERT(out.v.f > 2.99 && out.v.f < 3.01);
    free_fab_module(&c); uvm_destroy(&vm);
}

/* --- OP_MOVE --- */

/* Build LOADK R[0]=value; MOVE R[1]=R[0]; RET R[1]. */
static void fab_module_loadk_move_ret(UModule *c, int64_t value) {
    memset(c, 0, sizeof(*c));
    c->max_reg = 1;
    c->instructions = (uint32_t *)malloc(sizeof(uint32_t) * 3);
    c->instr_cap = 3;
    c->instr_count = 3;
    c->instructions[0] = uinstr_enc_abx(OP_LOADK, 0, 0);
    c->instructions[1] = uinstr_enc_abc(OP_MOVE, 1, 0, 0);
    c->instructions[2] = uinstr_enc_abc(OP_RET, 1, 0, 0);
    c->constants = (UValue *)malloc(sizeof(UValue) * 1);
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
    UModule c; fab_module_loadk_move_ret(&c, 99);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(99, out.v.i);
    free_fab_module(&c);
    uvm_destroy(&vm);
}

UTEST(vm_move_self_copy_is_noop) {
    /* LOADK R[0]=7; MOVE R[0]=R[0]; RET R[0]. */
    UModule c;
    memset(&c, 0, sizeof(c));
    c.max_reg = 0;
    c.instructions = (uint32_t *)malloc(sizeof(uint32_t) * 3);
    c.instr_cap = 3; c.instr_count = 3;
    c.instructions[0] = uinstr_enc_abx(OP_LOADK, 0, 0);
    c.instructions[1] = uinstr_enc_abc(OP_MOVE, 0, 0, 0);
    c.instructions[2] = uinstr_enc_abc(OP_RET, 0, 0, 0);
    c.constants = (UValue *)malloc(sizeof(UValue));
    c.const_cap = 1; c.const_count = 1;
    c.constants[0].kind = UVAL_INT; c.constants[0].v.i = 7;
    c.line_deltas = (int8_t *)malloc(sizeof(int8_t) * 3);
    c.line_deltas[0] = 1; c.line_deltas[1] = 0; c.line_deltas[2] = 0;

    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(7, out.v.i);
    free_fab_module(&c);
    uvm_destroy(&vm);
}

/* --- OP_NEG --- */

/* LOADK R[0]=value; NEG R[1]=R[0]; RET R[1]. */
static void fab_module_neg_int(UModule *c, int64_t value) {
    memset(c, 0, sizeof(*c));
    c->max_reg = 1;
    c->instructions = (uint32_t *)malloc(sizeof(uint32_t) * 3);
    c->instr_cap = 3; c->instr_count = 3;
    c->instructions[0] = uinstr_enc_abx(OP_LOADK, 0, 0);
    c->instructions[1] = uinstr_enc_abc(OP_NEG, 1, 0, 0);
    c->instructions[2] = uinstr_enc_abc(OP_RET, 1, 0, 0);
    c->constants = (UValue *)malloc(sizeof(UValue));
    c->const_cap = 1; c->const_count = 1;
    c->constants[0].kind = UVAL_INT; c->constants[0].v.i = value;
    c->line_deltas = (int8_t *)malloc(sizeof(int8_t) * 3);
    c->line_deltas[0] = 1; c->line_deltas[1] = 0; c->line_deltas[2] = 0;
}

UTEST(vm_neg_int_normal) {
    UModule c; fab_module_neg_int(&c, 5);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(-5, out.v.i);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_neg_int64_min_wraps_to_int64_min) {
    UModule c; fab_module_neg_int(&c, INT64_MIN);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT(out.v.i == INT64_MIN);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_neg_float) {
    UModule c; fab_module_neg_int(&c, 0);
    c.constants[0].kind = UVAL_FLOAT;
    c.constants[0].v.f = (URBI_FLOAT_TYPE == 8) ? 3.25 : (float)3.25;
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_FLOAT, out.kind);
    UASSERT(out.v.f > -3.26 && out.v.f < -3.24);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_neg_nil_is_type_error) {
    UModule c; fab_module_neg_int(&c, 0);
    c.constants[0].kind = UVAL_NIL;
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c, &out));
    free_fab_module(&c); uvm_destroy(&vm);
}

/* --- Diagnostic format tests --- */

UTEST(vm_type_error_diagnostic_binary_op) {
    UModule c; fab_module_add_mixed(&c, UVAL_BOOL, 1, 0, UVAL_INT, 5, 0);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c, &out));
    /* Without source_name, prefix is "line N:". line_deltas is [1, 0, 0, 0]
       (set by fab_module_int_add_int which fab_module_add_mixed uses as base),
       so ADD at pc=2 reports line 1. */
    UASSERT(strstr(vm.last_errmsg, "line 1:") != NULL);
    UASSERT(strstr(vm.last_errmsg, "TypeError") != NULL);
    UASSERT(strstr(vm.last_errmsg, "OP_ADD") != NULL);
    UASSERT(strstr(vm.last_errmsg, "Bool") != NULL);
    UASSERT(strstr(vm.last_errmsg, "Integer") != NULL);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_type_error_diagnostic_with_source_name) {
    UModule c; fab_module_add_mixed(&c, UVAL_BOOL, 1, 0, UVAL_INT, 5, 0);
    /* Allocate a source-name string via malloc+memcpy (strdup is POSIX, not
       C99; free_fab_module doesn't know about this field, so free explicitly). */
    const char src[] = "foo.u";
    c.source_name = (char *)malloc(sizeof(src));
    memcpy(c.source_name, src, sizeof(src));
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c, &out));
    UASSERT(strstr(vm.last_errmsg, "foo.u:1:") != NULL);
    free(c.source_name);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_type_error_diagnostic_unary_op) {
    UModule c; fab_module_neg_int(&c, 0);
    c.constants[0].kind = UVAL_NIL;
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c, &out));
    UASSERT(strstr(vm.last_errmsg, "OP_NEG") != NULL);
    UASSERT(strstr(vm.last_errmsg, "Nil") != NULL);
    UASSERT(strstr(vm.last_errmsg, "operand") != NULL);
    /* Singular "operand", not plural "operands" */
    UASSERT(strstr(vm.last_errmsg, "operands") == NULL);
    free_fab_module(&c); uvm_destroy(&vm);
}

UTEST(vm_type_error_diagnostic_no_synclines_uses_instr_prefix) {
    UModule c; fab_module_add_mixed(&c, UVAL_BOOL, 1, 0, UVAL_INT, 5, 0);
    /* Null out line_deltas to simulate a module built without syncline info. */
    free(c.line_deltas);
    c.line_deltas = NULL;
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c, &out));
    UASSERT(strstr(vm.last_errmsg, "instr 2:") != NULL);
    /* free_fab_module would double-free the already-freed line_deltas. */
    free(c.instructions); free(c.constants);
    uvm_destroy(&vm);
}

/* Allocator hook that always returns NULL. */
static void *uvm_alloc_always_null(void *ptr, size_t nbytes, void *ud) {
    (void)ptr; (void)nbytes; (void)ud;
    return NULL;
}

/* Allocator that returns NULL on first call (frame alloc), then
   forwards to stdlib (for the destroy path, which is a free = nbytes=0
   and safe to no-op too). Only used to exercise the first-alloc-fails
   path. */
static int uvm_alloc_fail_first_count = 0;
static void *uvm_alloc_fail_first(void *ptr, size_t nbytes, void *ud) {
    (void)ud;
    if (nbytes > 0 && uvm_alloc_fail_first_count == 0) {
        uvm_alloc_fail_first_count++;
        return NULL;  /* fail the frame allocation */
    }
    if (nbytes == 0) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

UTEST(vm_oom_returns_uvm_oom_with_diagnostic) {
    UModule c; fab_module_ret_only(&c, 0);
    UVM vm; uvm_init(&vm, uvm_alloc_always_null, NULL);
    UValue out;
    UASSERT_EQ(UVM_OOM, uvm_run(&vm, &c, &out));
    UASSERT_EQ(UVAL_NIL, out.kind);
    UASSERT(strstr(vm.last_errmsg, "out of memory") != NULL);
    UASSERT(strstr(vm.last_errmsg, "register frame") != NULL);
    /* frame size is (max_reg + 1) * sizeof(UValue) = 1 * 16 = 16 */
    UASSERT(strstr(vm.last_errmsg, "16") != NULL);
    free(c.instructions);
    free(c.line_deltas);
    uvm_destroy(&vm);
}

UTEST(vm_oom_first_alloc_fails_second_would_succeed) {
    uvm_alloc_fail_first_count = 0;  /* reset */
    UModule c; fab_module_ret_only(&c, 0);
    UVM vm; uvm_init(&vm, uvm_alloc_fail_first, NULL);
    UValue out;
    UASSERT_EQ(UVM_OOM, uvm_run(&vm, &c, &out));
    UASSERT(strstr(vm.last_errmsg, "out of memory") != NULL);
    free(c.instructions);
    free(c.line_deltas);
    uvm_destroy(&vm);
}

/* --- Coverage-completing tests (Task 16) --- */

/* OP_MUL TypeError path (lines 435-439 in uvm.c): Bool*Int is ill-typed. */
UTEST(vm_mul_bool_int_is_type_error) {
    UModule c; fab_module_add_mixed(&c, UVAL_BOOL, 1, 0, UVAL_INT, 3, 0);
    c.instructions[2] = uinstr_enc_abc(OP_MUL, 2, 0, 1);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c, &out));
    UASSERT(strstr(vm.last_errmsg, "OP_MUL") != NULL);
    UASSERT(strstr(vm.last_errmsg, "Bool") != NULL);
    free_fab_module(&c); uvm_destroy(&vm);
}

/* OP_DIV TypeError path (lines 450-454 in uvm.c): Bool/Int is ill-typed. */
UTEST(vm_div_bool_int_is_type_error) {
    UModule c; fab_module_add_mixed(&c, UVAL_BOOL, 1, 0, UVAL_INT, 3, 0);
    c.instructions[2] = uinstr_enc_abc(OP_DIV, 2, 0, 1);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c, &out));
    UASSERT(strstr(vm.last_errmsg, "OP_DIV") != NULL);
    free_fab_module(&c); uvm_destroy(&vm);
}

/* kind_name "Float" path (line 132): Float operand in a binary TypeError.
   Float+Bool: b=Float (number), c=Bool (not number) → error; b_kind=UVAL_FLOAT. */
UTEST(vm_add_float_bool_diagnostic_shows_float_kind) {
    UModule c; fab_module_add_mixed(&c, UVAL_FLOAT, 0, 1.0, UVAL_BOOL, 1, 0);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c, &out));
    UASSERT(strstr(vm.last_errmsg, "Float") != NULL);
    free_fab_module(&c); uvm_destroy(&vm);
}

/* kind_name "String" path (line 134): fabricate a UVAL_STR constant as
   one operand; triggers a TypeError that prints "String". */
UTEST(vm_add_string_int_diagnostic_shows_string_kind) {
    UModule c; fab_module_add_mixed(&c, UVAL_INT, 5, 0, UVAL_INT, 3, 0);
    c.constants[0].kind = UVAL_STR;  /* override to String */
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c, &out));
    UASSERT(strstr(vm.last_errmsg, "String") != NULL);
    free_fab_module(&c); uvm_destroy(&vm);
}

/* kind_name "unknown" path (line 136): use an out-of-range kind value.
   This exercises the default fallback in kind_name. */
UTEST(vm_add_unknown_kind_diagnostic_shows_unknown) {
    UModule c; fab_module_add_mixed(&c, UVAL_INT, 5, 0, UVAL_INT, 3, 0);
    c.constants[0].kind = 99;  /* out-of-range — unreachable in normal use */
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c, &out));
    UASSERT(strstr(vm.last_errmsg, "unknown") != NULL);
    free_fab_module(&c); uvm_destroy(&vm);
}

/* diag_write_u32 zero branch (line 196): type error at pc=0 with no synclines
   produces "instr 0: ..." which calls diag_write_u32(w, 0). */
UTEST(vm_type_error_at_pc_zero_writes_instr_zero) {
    /* Build: LOADK R[0]=Bool, LOADK R[1]=Int not needed — just ADD at pc=0
       directly using a 1-instruction module with Bool in both slots. */
    UModule c;
    memset(&c, 0, sizeof(c));
    c.max_reg = 2;
    c.instructions = (uint32_t *)malloc(sizeof(uint32_t) * 1);
    c.instr_cap = 1; c.instr_count = 1;
    c.instructions[0] = uinstr_enc_abc(OP_ADD, 0, 1, 2);
    /* No constants needed — frame is zero-initialized to Nil, so
       ADD R[0]=R[1]+R[2] where R[1] and R[2] are Nil → TypeError at pc=0. */
    /* No line_deltas (NULL) → prefix falls back to "instr 0:". */
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c, &out));
    UASSERT(strstr(vm.last_errmsg, "instr 0:") != NULL);
    free(c.instructions);
    uvm_destroy(&vm);
}

/* DiagWriter truncation path (lines 176-184): a source_name long enough to
   push past the UVM_ERRMSG_CAP boundary. The message ends with "...". */
UTEST(vm_type_error_diagnostic_truncates_to_ellipsis) {
    UModule c; fab_module_add_mixed(&c, UVAL_BOOL, 1, 0, UVAL_INT, 5, 0);
    /* 110-character source name exceeds buffer capacity (128 bytes total),
       forcing truncation. malloc+memcpy avoids strdup (POSIX, not C99). */
    const char *long_name =
        "very_long_path/that/intentionally/exceeds/the/fixed/"
        "error/message/buffer/capacity/to/trigger/truncation";
    size_t name_len = strlen(long_name) + 1;
    c.source_name = (char *)malloc(name_len);
    memcpy(c.source_name, long_name, name_len);
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c, &out));
    size_t msg_len = strlen(vm.last_errmsg);
    UASSERT(msg_len > 0);
    UASSERT(msg_len < UVM_ERRMSG_CAP);
    UASSERT(vm.last_errmsg[msg_len - 3] == '.' &&
            vm.last_errmsg[msg_len - 2] == '.' &&
            vm.last_errmsg[msg_len - 1] == '.');
    free(c.source_name);
    free_fab_module(&c); uvm_destroy(&vm);
}

/* vm_line_for_pc abs_lines branch (lines 234-241): build a module whose
   line_deltas[0] == INT8_MIN (abs checkpoint sentinel) with a matching
   abs_lines entry. The resulting diagnostic prefix uses "line N:" from
   the abs checkpoint rather than summing deltas. */
UTEST(vm_line_for_pc_abs_checkpoint_used_in_diagnostic) {
    UModule c; fab_module_add_mixed(&c, UVAL_BOOL, 1, 0, UVAL_INT, 5, 0);
    /* Override line_deltas: set delta[0..3] = INT8_MIN for all 4 instrs so
       every instruction references an abs checkpoint. */
    free(c.line_deltas);
    c.line_deltas = (int8_t *)malloc(sizeof(int8_t) * 4);
    c.line_deltas[0] = INT8_MIN;
    c.line_deltas[1] = INT8_MIN;
    c.line_deltas[2] = INT8_MIN;
    c.line_deltas[3] = INT8_MIN;
    /* Allocate abs_lines with 4 entries — one per instruction. */
    c.abs_lines = (AbsLine *)malloc(sizeof(AbsLine) * 4);
    c.abs_line_count = 4;
    c.abs_lines[0].pc = 0; c.abs_lines[0].line = 10;
    c.abs_lines[1].pc = 1; c.abs_lines[1].line = 11;
    c.abs_lines[2].pc = 2; c.abs_lines[2].line = 12;
    c.abs_lines[3].pc = 3; c.abs_lines[3].line = 13;
    UVM vm; uvm_init(&vm, NULL, NULL);
    UValue out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c, &out));
    /* ADD is at pc=2; abs_lines[2].line == 12 → prefix "line 12:". */
    UASSERT(strstr(vm.last_errmsg, "line 12:") != NULL);
    free_fab_module(&c); uvm_destroy(&vm);
}

/* --- uvm_run entry-state reset --- */

UTEST(vm_run_resets_last_error_on_successful_run) {
    /* First run fails with TypeError; second run succeeds; last_error
       and last_errmsg should reflect only the second run. */
    UVM vm; uvm_init(&vm, NULL, NULL);

    /* Run 1 — force TypeError. */
    UModule c1; fab_module_add_mixed(&c1, UVAL_BOOL, 1, 0, UVAL_INT, 5, 0);
    UValue out;
    UASSERT_EQ(UVM_TYPE_ERROR, uvm_run(&vm, &c1, &out));
    UASSERT_EQ(UVM_TYPE_ERROR, vm.last_error);
    UASSERT(vm.last_errmsg[0] != '\0');
    free_fab_module(&c1);

    /* Run 2 — succeeds. last_error and last_errmsg should be reset. */
    UModule c2; fab_module_loadk_int_ret(&c2, 42);
    UASSERT_EQ(UVM_OK, uvm_run(&vm, &c2, &out));
    UASSERT_EQ(UVM_OK, vm.last_error);
    UASSERT_EQ('\0', vm.last_errmsg[0]);
    UASSERT_EQ(UVAL_INT, out.kind);
    UASSERT_EQ(42, out.v.i);
    free_fab_module(&c2);

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
    utest_run("uvm_run on empty module returns Nil",
              vm_run_empty_module_returns_nil);
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
    utest_run("uvm OP_SUB Int-Int normal", vm_sub_int_int_normal);
    utest_run("uvm OP_SUB INT64_MIN-1 wraps to INT64_MAX",
              vm_sub_int_int_min_minus_one_wraps);
    utest_run("uvm OP_SUB Float-Float", vm_sub_float_float);
    utest_run("uvm OP_SUB Bool operand raises TypeError",
              vm_sub_bool_operand_is_type_error);
    utest_run("uvm OP_MUL Int*Int normal", vm_mul_int_int_normal);
    utest_run("uvm OP_MUL INT64_MIN*-1 wraps to INT64_MIN",
              vm_mul_int_int_min_times_neg_one_wraps_to_min);
    utest_run("uvm OP_MUL Float*Int promotes", vm_mul_float_int_promotes);
    utest_run("uvm OP_DIV Int/Int produces Float (never Integer)",
              vm_div_int_int_always_float);
    utest_run("uvm OP_DIV Int/Int exact still returns Float",
              vm_div_int_int_exact_still_float);
    utest_run("uvm OP_DIV positive/0 is +Inf",
              vm_div_by_zero_positive_is_inf);
    utest_run("uvm OP_DIV negative/0 is -Inf",
              vm_div_by_zero_negative_is_neg_inf);
    utest_run("uvm OP_DIV 0/0 is NaN", vm_div_zero_by_zero_is_nan);
    utest_run("uvm OP_DIV Float/Float", vm_div_float_float);
    utest_run("uvm OP_NEG Int normal", vm_neg_int_normal);
    utest_run("uvm OP_NEG INT64_MIN wraps to INT64_MIN (no UB)",
              vm_neg_int64_min_wraps_to_int64_min);
    utest_run("uvm OP_NEG Float", vm_neg_float);
    utest_run("uvm OP_NEG Nil raises TypeError", vm_neg_nil_is_type_error);
    utest_run("uvm TypeError diagnostic for binary op includes line + kinds",
              vm_type_error_diagnostic_binary_op);
    utest_run("uvm TypeError diagnostic with source_name uses source:line:",
              vm_type_error_diagnostic_with_source_name);
    utest_run("uvm TypeError diagnostic for unary op uses 'operand' not 'operands'",
              vm_type_error_diagnostic_unary_op);
    utest_run("uvm TypeError diagnostic without synclines uses 'instr N:'",
              vm_type_error_diagnostic_no_synclines_uses_instr_prefix);
    utest_run("uvm OOM returns UVM_OOM with 'out of memory' diagnostic",
              vm_oom_returns_uvm_oom_with_diagnostic);
    utest_run("uvm OOM first alloc fails produces diagnostic",
              vm_oom_first_alloc_fails_second_would_succeed);
    utest_run("uvm OP_MUL Bool*Int raises TypeError",
              vm_mul_bool_int_is_type_error);
    utest_run("uvm OP_DIV Bool/Int raises TypeError",
              vm_div_bool_int_is_type_error);
    utest_run("uvm ADD Float+Bool diagnostic shows Float kind",
              vm_add_float_bool_diagnostic_shows_float_kind);
    utest_run("uvm ADD String+Int diagnostic shows String kind",
              vm_add_string_int_diagnostic_shows_string_kind);
    utest_run("uvm ADD unknown kind diagnostic shows 'unknown'",
              vm_add_unknown_kind_diagnostic_shows_unknown);
    utest_run("uvm TypeError at pc=0 without synclines shows 'instr 0:'",
              vm_type_error_at_pc_zero_writes_instr_zero);
    utest_run("uvm TypeError diagnostic truncates with '...' when message too long",
              vm_type_error_diagnostic_truncates_to_ellipsis);
    utest_run("uvm vm_line_for_pc abs checkpoint used in diagnostic",
              vm_line_for_pc_abs_checkpoint_used_in_diagnostic);
    utest_run("uvm_run resets last_error/last_errmsg on entry",
              vm_run_resets_last_error_on_successful_run);
}
