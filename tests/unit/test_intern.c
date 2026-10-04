/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include <stdio.h>
#include <string.h>

#include "emit/uintern.h"
#include "urbi/urbi.h"

#define UTEST(name) static void name(void)

/* The intern table is the VM's own; the standard library is not needed
 * to exercise it, so these VMs open without booting it. */
static const UVMConfig no_stdlib = { 0, 0, 0 };

UTEST(intern_returns_canonical_pointer) {
    UVM *vm = NULL;
    vm = urbi_open(utest_alloc, NULL, &no_stdlib);

    const char *a = ustr_intern(vm, "hello", 5);
    const char *b = ustr_intern(vm, "hello", 5);
    UASSERT(a != NULL);
    UASSERT(a == b);                /* pointer equality */
    UASSERT_EQ(0, strcmp(a, "hello"));

    urbi_close(vm);
}

UTEST(intern_distinguishes_different_strings) {
    UVM *vm = NULL;
    vm = urbi_open(utest_alloc, NULL, &no_stdlib);

    const char *a = ustr_intern(vm, "foo", 3);
    const char *b = ustr_intern(vm, "bar", 3);
    UASSERT(a != b);

    urbi_close(vm);
}

UTEST(intern_treats_substrings_as_distinct) {
    UVM *vm = NULL;
    vm = urbi_open(utest_alloc, NULL, &no_stdlib);

    const char *full = ustr_intern(vm, "foobar", 6);
    const char *part = ustr_intern(vm, "foo", 3);
    UASSERT(full != part);

    urbi_close(vm);
}

UTEST(intern_handles_zero_length) {
    UVM *vm = NULL;
    vm = urbi_open(utest_alloc, NULL, &no_stdlib);
    const char *empty = ustr_intern(vm, "", 0);
    UASSERT(empty != NULL);
    UASSERT_EQ((char)0, empty[0]);
    urbi_close(vm);
}


UTEST(intern_two_vms_have_independent_tables) {
    UVM *vm_a = urbi_open(utest_alloc, NULL, &no_stdlib);
    UVM *vm_b = urbi_open(utest_alloc, NULL, &no_stdlib);

    const char *sa = ustr_intern(vm_a, "shared", 6);
    const char *sb = ustr_intern(vm_b, "shared", 6);
    UASSERT(sa != NULL);
    UASSERT(sb != NULL);
    UASSERT(sa != sb);                /* per-VM table = per-VM canonical pointer */
    UASSERT_EQ(0, strcmp(sa, sb));

    urbi_close(vm_a);
    urbi_close(vm_b);
}




void test_intern_suite(void) {
    utest_run("intern returns canonical pointer", intern_returns_canonical_pointer);
    utest_run("intern distinguishes different strings", intern_distinguishes_different_strings);
    utest_run("intern treats substrings as distinct", intern_treats_substrings_as_distinct);
    utest_run("intern handles zero length", intern_handles_zero_length);
    utest_run("intern two VMs have independent tables", intern_two_vms_have_independent_tables);
}
