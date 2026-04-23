/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include "uvm.h"
#include <string.h>

#define UTEST(name) static void name(void)

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
    uvm_init(&vm, (UVMAllocFn)(void *)0xDEADBEEFull, (void *)0xCAFEBABEull);
    UASSERT(vm.alloc_fn == (UVMAllocFn)(void *)0xDEADBEEFull);
    UASSERT(vm.alloc_ud == (void *)0xCAFEBABEull);
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
}
