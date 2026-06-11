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
    UASSERT_EQ((size_t)0, urbi_intern_bytes(&vm));   /* no table → 0 */
}

UTEST(intern_bytes_grows_on_miss_not_on_hit) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Bare urbi_vm_init interns nothing — the table is created lazily on
     * the first ustr_intern call, so the byte counter starts at zero. */
    UASSERT_EQ((size_t)0, urbi_intern_bytes(&vm));

    /* Fresh 10-byte string: counter grows by at least the payload length.
     * (Header + entries-array bytes are counted too; assert the monotonic
     * invariant, not an exact layout constant.) */
    const char *a = ustr_intern(&vm, "tenbytes..", 10);
    UASSERT(a != NULL);
    size_t after_first = urbi_intern_bytes(&vm);
    UASSERT(after_first >= (size_t)10);

    /* Same string again: dedup hit, zero new allocation. */
    const char *b = ustr_intern(&vm, "tenbytes..", 10);
    UASSERT(a == b);
    UASSERT_EQ(after_first, urbi_intern_bytes(&vm));

    urbi_vm_destroy(&vm);
}

UTEST(intern_bytes_consistent_across_grows) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* 100 distinct 5-byte strings force several table grows.  Each miss
     * must grow the counter by at least the payload length, including
     * across rehash (old entries array is subtracted, new one added). */
    char buf[16];
    size_t prev = urbi_intern_bytes(&vm);
    size_t payload_total = 0;
    for (int i = 0; i < 100; i++) {
        int len = snprintf(buf, sizeof buf, "k%04d", i);
        UASSERT(ustr_intern(&vm, buf, (size_t)len) != NULL);
        size_t now = urbi_intern_bytes(&vm);
        UASSERT(now >= prev + (size_t)len);
        prev = now;
        payload_total += (size_t)len;
    }
    UASSERT(prev >= payload_total);

    /* Re-intern all 100: pure hits, counter unchanged. */
    for (int i = 0; i < 100; i++) {
        int len = snprintf(buf, sizeof buf, "k%04d", i);
        UASSERT(ustr_intern(&vm, buf, (size_t)len) != NULL);
    }
    UASSERT_EQ(prev, urbi_intern_bytes(&vm));

    urbi_vm_destroy(&vm);
}

void test_intern_suite(void) {
    utest_run("intern returns canonical pointer", intern_returns_canonical_pointer);
    utest_run("intern distinguishes different strings", intern_distinguishes_different_strings);
    utest_run("intern treats substrings as distinct", intern_treats_substrings_as_distinct);
    utest_run("intern handles zero length", intern_handles_zero_length);
    utest_run("intern grows through load factor", intern_grows_through_load_factor);
    utest_run("intern two VMs have independent tables", intern_two_vms_have_independent_tables);
    utest_run("intern destroy is safe on zero init", intern_destroy_is_safe_on_zero_init);
    utest_run("intern bytes grows on miss not on hit", intern_bytes_grows_on_miss_not_on_hit);
    utest_run("intern bytes consistent across grows", intern_bytes_consistent_across_grows);
}
