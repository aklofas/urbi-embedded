/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include <stdio.h>
#include <string.h>

#include "value/uintern.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

UTEST(intern_returns_canonical_pointer) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    const char *a = ustr_intern(&vm, "hello", 5);
    const char *b = ustr_intern(&vm, "hello", 5);
    UASSERT(a != NULL);
    UASSERT(a == b);                /* pointer equality */
    UASSERT_EQ(0, strcmp(a, "hello"));
    UASSERT_EQ((size_t)1, uintern_count(&vm));

    urbi_vm_destroy(&vm);
}

UTEST(intern_distinguishes_different_strings) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    const char *a = ustr_intern(&vm, "foo", 3);
    const char *b = ustr_intern(&vm, "bar", 3);
    UASSERT(a != b);
    UASSERT_EQ((size_t)2, uintern_count(&vm));

    urbi_vm_destroy(&vm);
}

UTEST(intern_treats_substrings_as_distinct) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    const char *full = ustr_intern(&vm, "foobar", 6);
    const char *part = ustr_intern(&vm, "foo", 3);
    UASSERT(full != part);
    UASSERT_EQ((size_t)2, uintern_count(&vm));

    urbi_vm_destroy(&vm);
}

UTEST(intern_handles_zero_length) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    const char *empty = ustr_intern(&vm, "", 0);
    UASSERT(empty != NULL);
    UASSERT_EQ((char)0, empty[0]);
    UASSERT_EQ((size_t)1, uintern_count(&vm));
    urbi_vm_destroy(&vm);
}

UTEST(intern_grows_through_load_factor) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Insert 100 distinct strings — forces multiple grows. */
    char buf[16];
    for (int i = 0; i < 100; i++) {
        int len = snprintf(buf, sizeof buf, "k%04d", i);
        const char *s = ustr_intern(&vm, buf, (size_t)len);
        UASSERT(s != NULL);
    }
    UASSERT_EQ((size_t)100, uintern_count(&vm));

    /* Re-insert all 100 — should be stable pointers, no growth. */
    for (int i = 0; i < 100; i++) {
        int len = snprintf(buf, sizeof buf, "k%04d", i);
        const char *s = ustr_intern(&vm, buf, (size_t)len);
        UASSERT(s != NULL);
    }
    UASSERT_EQ((size_t)100, uintern_count(&vm));

    urbi_vm_destroy(&vm);
}

UTEST(intern_two_vms_have_independent_tables) {
    UVM vm_a, vm_b;
    urbi_vm_init(&vm_a, NULL, NULL);
    urbi_vm_init(&vm_b, NULL, NULL);

    const char *sa = ustr_intern(&vm_a, "shared", 6);
    const char *sb = ustr_intern(&vm_b, "shared", 6);
    UASSERT(sa != NULL);
    UASSERT(sb != NULL);
    UASSERT(sa != sb);                /* per-VM table = per-VM canonical pointer */
    UASSERT_EQ(0, strcmp(sa, sb));

    urbi_vm_destroy(&vm_a);
    urbi_vm_destroy(&vm_b);
}

UTEST(intern_destroy_is_safe_on_zero_init) {
    UVM vm = {0};
    uintern_destroy(&vm);             /* must not crash */
    UASSERT(vm.intern_table == NULL);
}

void test_intern_suite(void) {
    utest_run("intern returns canonical pointer", intern_returns_canonical_pointer);
    utest_run("intern distinguishes different strings", intern_distinguishes_different_strings);
    utest_run("intern treats substrings as distinct", intern_treats_substrings_as_distinct);
    utest_run("intern handles zero length", intern_handles_zero_length);
    utest_run("intern grows through load factor", intern_grows_through_load_factor);
    utest_run("intern two VMs have independent tables", intern_two_vms_have_independent_tables);
    utest_run("intern destroy is safe on zero init", intern_destroy_is_safe_on_zero_init);
}
