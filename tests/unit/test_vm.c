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
}
