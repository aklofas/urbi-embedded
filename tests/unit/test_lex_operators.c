/* SPDX-License-Identifier: BSD-3-Clause */
/* test_lex_operators.c — v1.0-rc stdlib-completeness lexer tests for the
 * %, &&, || operators.
 *
 * Guards:
 *   1. `&` / `|` still lex as the single-char statement separators
 *      (TOK_AMP / TOK_PIPE) after being moved out of the punct fast-path.
 *   2. `&&` / `||` lex as the new TOK_AMPAMP / TOK_PIPEPIPE.
 *   3. `%` lexes as the new TOK_PERCENT.
 *
 * Also carries the && short-circuit side-effect end-to-end test (Task 3):
 *   `false && f()` must NOT evaluate f().
 */

#include "utest.h"
#include "utest_e2e_helpers.h"
#include "lex/ulex.h"
#include "runtime/umacros.h"   /* urbi_strlen */
#include "vm/uvm.h"
#include "urbi/urbi.h"

#define UTEST(name) static void name(void)

/* Return the n-th token type (0-based) of src. */
static UTokenType nth_tok(const char *src, int n) {
    ULexer lx;
    ulex_init(&lx, src, (size_t)urbi_strlen(src));
    UToken t;
    int i = 0;
    do { t = ulex_next(&lx); } while (i++ < n);
    return t.type;
}

UTEST(lex_amp_still_separator)   { UASSERT_EQ((int)nth_tok("a & b", 1),  (int)TOK_AMP); }
UTEST(lex_pipe_still_separator)  { UASSERT_EQ((int)nth_tok("a | b", 1),  (int)TOK_PIPE); }
UTEST(lex_ampamp)                { UASSERT_EQ((int)nth_tok("a && b", 1), (int)TOK_AMPAMP); }
UTEST(lex_pipepipe)              { UASSERT_EQ((int)nth_tok("a || b", 1), (int)TOK_PIPEPIPE); }
UTEST(lex_percent)               { UASSERT_EQ((int)nth_tok("a % b", 1),  (int)TOK_PERCENT); }

/* && / || are two chars; the trailing single separators must be unaffected:
 * `a & b & c` -> idents and AMP separators, never AMPAMP. */
UTEST(lex_single_amp_run_no_ampamp) {
    UASSERT_EQ((int)nth_tok("a & b & c", 1), (int)TOK_AMP);
    UASSERT_EQ((int)nth_tok("a & b & c", 3), (int)TOK_AMP);
}

/* Task 3: && short-circuits — RHS side effect must not fire when LHS is false. */
UTEST(logical_short_circuits_and) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out;
    int rc = utest_e2e_compile_and_run(&vm,
        "var Realm.hit = 0;"
        " var f = function() { Realm.hit = 1; true };"
        " false && f();"
        " Realm.hit",
        &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(0, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

/* Task 3: || short-circuits — RHS side effect must not fire when LHS is true. */
UTEST(logical_short_circuits_or) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out;
    int rc = utest_e2e_compile_and_run(&vm,
        "var Realm.hit = 0;"
        " var f = function() { Realm.hit = 1; false };"
        " true || f();"
        " Realm.hit",
        &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(0, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

void test_lex_operators_suite(void);
void test_lex_operators_suite(void) {
    utest_run("lex_amp_still_separator",        lex_amp_still_separator);
    utest_run("lex_pipe_still_separator",       lex_pipe_still_separator);
    utest_run("lex_ampamp",                     lex_ampamp);
    utest_run("lex_pipepipe",                   lex_pipepipe);
    utest_run("lex_percent",                    lex_percent);
    utest_run("lex_single_amp_run_no_ampamp",   lex_single_amp_run_no_ampamp);
    utest_run("logical_short_circuits_and",     logical_short_circuits_and);
    utest_run("logical_short_circuits_or",      logical_short_circuits_or);
}
