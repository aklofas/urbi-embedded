/* SPDX-License-Identifier: BSD-3-Clause */
/* test_string_literal_e2e.c — string literals end-to-end:
 * lex → parse → emit → execute → register holds UVAL_STR.
 *
 * Phase 1: closes the full happy path for string literals.  The VM's
 * OP_LOADK arm handles arbitrary UValue kinds (raw constant-pool copy
 * into the destination register), so no VM-side change is required —
 * this suite exercises that the path actually works under realistic
 * compile-and-run pressure. */

#include "utest.h"
#include "value/uvalue.h"
#include "value/uintern.h"
#include "vm/uvm.h"
#include "module/umodule.h"
#include "realm/urealm.h"
#include "utest_e2e_helpers.h"
#include <string.h>

static void e2e_string_in_var(void) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue result = {0};
    int rc = utest_e2e_compile_and_run(&vm, "var s = \"hello\"", &result);
    UASSERT_EQ(URBI_OK, rc);

    /* Read s back from the realm globals. */
    UValue v = {0};
    int g = urbi_realm_get_global(&vm, urbi_realm_global(&vm),
                                  "s", 1, &v);
    UASSERT_EQ(URBI_OK, g);
    UASSERT_EQ((uint8_t)UVAL_STR, v.kind);
    const char *got = (const char *)v.v.p;
    UASSERT(got != NULL);
    UASSERT(memcmp(got, "hello", 5) == 0);

    urbi_vm_destroy(&vm);
}

static void e2e_adjacent_strings_concat(void) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue result = {0};
    int rc = utest_e2e_compile_and_run(&vm,
        "var s = \"a\" \"b\" \"c\"", &result);
    UASSERT_EQ(URBI_OK, rc);

    UValue v = {0};
    int g = urbi_realm_get_global(&vm, urbi_realm_global(&vm),
                                  "s", 1, &v);
    UASSERT_EQ(URBI_OK, g);
    UASSERT_EQ((uint8_t)UVAL_STR, v.kind);
    const char *got = (const char *)v.v.p;
    UASSERT(got != NULL);
    UASSERT(memcmp(got, "abc", 3) == 0);

    urbi_vm_destroy(&vm);
}

static void e2e_string_equality_via_intern(void) {
    /* Two string literals with the same content intern to the same pointer;
     * `==` evaluates to true (UVAL_STR equality is pointer-compare per
     * uvalue_equal in src/value/uvalue.c). */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue result = {0};
    int rc = utest_e2e_compile_and_run(&vm,
        "var eq = \"foo\" == \"foo\"", &result);
    UASSERT_EQ(URBI_OK, rc);

    UValue v = {0};
    int g = urbi_realm_get_global(&vm, urbi_realm_global(&vm),
                                  "eq", 2, &v);
    UASSERT_EQ(URBI_OK, g);
    UASSERT_EQ((uint8_t)UVAL_BOOL, v.kind);
    UASSERT(v.v.i != 0);   /* true */

    urbi_vm_destroy(&vm);
}

static void e2e_string_with_escapes(void) {
    /* Resolved bytes for "a\nb" are 'a', 0x0A, 'b'.  Verify the resolved
     * content matches a manually interned reference. */
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UValue result = {0};
    int rc = utest_e2e_compile_and_run(&vm, "var s = \"a\\nb\"", &result);
    UASSERT_EQ(URBI_OK, rc);

    UValue v = {0};
    int g = urbi_realm_get_global(&vm, urbi_realm_global(&vm),
                                  "s", 1, &v);
    UASSERT_EQ(URBI_OK, g);
    UASSERT_EQ((uint8_t)UVAL_STR, v.kind);
    const char *got = (const char *)v.v.p;
    UASSERT(got != NULL);
    UASSERT_EQ('a', got[0]);
    UASSERT_EQ('\n', got[1]);
    UASSERT_EQ('b', got[2]);

    urbi_vm_destroy(&vm);
}

void test_string_literal_e2e_suite(void) {
    utest_run("e2e_string_in_var", e2e_string_in_var);
    utest_run("e2e_adjacent_strings_concat", e2e_adjacent_strings_concat);
    utest_run("e2e_string_equality_via_intern", e2e_string_equality_via_intern);
    utest_run("e2e_string_with_escapes", e2e_string_with_escapes);
}
