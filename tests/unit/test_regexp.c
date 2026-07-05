/* SPDX-License-Identifier: BSD-3-Clause */
/* test_regexp.c — RegExp matcher + type wiring (v1.0 stdlib-completeness).
 *
 * Exercises the compact backtracking matcher directly (urbi_regexp_search)
 * across literals, `.`, `*`, `+`, `?`, `^`, `$`, and char classes, plus the
 * end-to-end RegExp.new / .test / .match surface.
 *
 * Budget tests (STD-04): catastrophic backtracking pattern returns -1 (budget
 * exceeded) rather than spinning; depth-bomb pattern hits depth cap rather
 * than overflowing the C stack on embedded targets; the script-visible surface
 * raises a catchable RangeError when the budget is exhausted. */

#include "utest.h"
#include "stdlib/regexp.h"
#include "utest_e2e_helpers.h"
#include "vm/uvm.h"
#include "urbi/urbi.h"
#include "urbi/types.h"   /* URBI_ERR_UNCAUGHT_THROW */
#include "chunk/uchunk.h"
#include "runtime/umacros.h"

#define UTEST(name) static void name(void)

/* Convenience wrapper: search NUL-terminated `pat` in NUL-terminated `s`. */
static int
re(const char *pat, const char *s)
{
    return urbi_regexp_search(pat, urbi_strlen(pat), s, s + urbi_strlen(s));
}

/* --- matcher: literals + anchors ---------------------------------------- */

UTEST(re_literal_match)        { UASSERT(re("abc", "abc")); }
UTEST(re_literal_substring)    { UASSERT(re("bc", "abcd")); }
UTEST(re_literal_no_match)     { UASSERT(!re("xyz", "abc")); }
UTEST(re_dot_any)              { UASSERT(re("a.c", "axc")); }
UTEST(re_dot_no_match_empty)   { UASSERT(!re("a.c", "ac")); }
UTEST(re_anchor_start)         { UASSERT(re("^ab", "abc")); }
UTEST(re_anchor_start_fail)    { UASSERT(!re("^bc", "abc")); }
UTEST(re_anchor_end)           { UASSERT(re("bc$", "abc")); }
UTEST(re_anchor_end_fail)      { UASSERT(!re("ab$", "abc")); }
UTEST(re_anchor_both)          { UASSERT(re("^a.c$", "abc")); }
UTEST(re_anchor_both_fail)     { UASSERT(!re("^a.c$", "abbc")); }
UTEST(re_empty_matches)        { UASSERT(re("", "anything")); }

/* --- matcher: quantifiers ------------------------------------------------ */

UTEST(re_star_zero)            { UASSERT(re("ab*c", "ac")); }
UTEST(re_star_many)            { UASSERT(re("ab*c", "abbbc")); }
UTEST(re_plus_one)             { UASSERT(re("ab+c", "abc")); }
UTEST(re_plus_zero_fail)       { UASSERT(!re("ab+c", "ac")); }
UTEST(re_question_present)     { UASSERT(re("ab?c", "abc")); }
UTEST(re_question_absent)      { UASSERT(re("ab?c", "ac")); }
UTEST(re_dotstar)              { UASSERT(re("a.*z", "abcdz")); }
UTEST(re_greedy_backtrack)     { UASSERT(re("^a.*c$", "abcxc")); }

/* --- matcher: char classes ----------------------------------------------- */

UTEST(re_class_member)         { UASSERT(re("[abc]", "x b y")); }
UTEST(re_class_range)          { UASSERT(re("[0-9]+", "x42y")); }
UTEST(re_class_range_fail)     { UASSERT(!re("[0-9]+", "xyz")); }
UTEST(re_class_negated)        { UASSERT(re("[^0-9]", "12a34")); }
UTEST(re_class_negated_fail)   { UASSERT(!re("^[^0-9]+$", "abc1")); }
UTEST(re_class_anchored)       { UASSERT(re("^[a-z]+$", "hello")); }
UTEST(re_class_anchored_fail)  { UASSERT(!re("^[a-z]+$", "Hello")); }

/* --- end-to-end: RegExp type --------------------------------------------- */

UTEST(re_e2e_test_true)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out;
    int rc = utest_e2e_compile_and_run(&vm,
        "RegExp.new(\"^a.c$\").test(\"abc\")", &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_BOOL, (int)out.kind);
    UASSERT_EQ(1, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

UTEST(re_e2e_test_false)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out;
    int rc = utest_e2e_compile_and_run(&vm,
        "RegExp.new(\"[0-9]+\").test(\"xyz\")", &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_BOOL, (int)out.kind);
    UASSERT_EQ(0, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

UTEST(re_e2e_match_alias)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out;
    int rc = utest_e2e_compile_and_run(&vm,
        "RegExp.new(\"ab*c\").match(\"ac\")", &out);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_BOOL, (int)out.kind);
    UASSERT_EQ(1, (int)out.v.i);
    urbi_vm_destroy(&vm);
}

/* --- budget: step exhaustion (catastrophic backtracking) ----------------- */

/* urbi_regexp_search returns -1 when the step budget is exhausted.
 * Pattern a*a*a*a*a*a*b against 25 a's with no b: the six overlapping *
 * quantifiers create C(n+5,5) ~ n^5/120 backtrack paths per start position;
 * 25 chars × ~25^5/120 ≈ 1.3M paths exceeds the 1M-step budget.
 * TDD RED: pre-fix urbi_regexp_search returns 0 (no match), not -1. */
UTEST(re_budget_steps_exhausted)
{
    const char *pat = "a*a*a*a*a*a*b";
    const char *s   = "aaaaaaaaaaaaaaaaaaaaaaaaa"; /* 25 a's, no b */
    int r = urbi_regexp_search(pat, urbi_strlen(pat), s, s + urbi_strlen(s));
    UASSERT_EQ(-1, r);  /* budget exceeded, not 0 (no match) */
}

/* Budget: recursion depth cap protects C call stack on embedded targets.
 * 200 'a?' quantifiers require 200 nested re_match_here frames.  Without
 * the depth cap this would overflow a 64 KB MCU stack (200 × ~64 B = 12 KB
 * consumed vs. typical 4-8 KB free headroom).  With cap = 128 it returns -1
 * at depth 129.
 * TDD RED: pre-fix urbi_regexp_search returns 0 or 1, not -1. */
UTEST(re_budget_depth_exhausted)
{
    /* 200 'a?' pairs = 400 pattern bytes; triggers depth cap of 128. */
    char pat[401];
    size_t i;
    for (i = 0U; i < 200U; i++) { pat[2U * i] = 'a'; pat[2U * i + 1U] = '?'; }
    pat[400] = '\0';
    const char *s = "a";
    int r = urbi_regexp_search(pat, 400U, s, s + 1U);
    UASSERT_EQ(-1, r);  /* depth cap, not normal 0/1 result */
}

/* Budget: script-visible throw on exhaustion (uncaught-throw path).
 * The catastrophic pattern exhausts the budget → regexp_do_test calls
 * urbi_raise_range → native call returns UEXEC_THROW → urbi_run_chunk
 * returns URBI_ERR_UNCAUGHT_THROW (-18).  The isA(RangeError) contract
 * is verified in tests/chk/stdlib/runtime/regexp.chk (entry [00000006]).
 * TDD RED: pre-fix rc == URBI_OK (no throw). */
UTEST(re_e2e_budget_raises)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UValue out = urbi_make_nil();
    int rc = utest_e2e_compile_and_run(&vm,
        "RegExp.new(\"a*a*a*a*a*a*b\").test(\"aaaaaaaaaaaaaaaaaaaaaaaaa\")", &out);
    UASSERT_EQ(URBI_ERR_UNCAUGHT_THROW, rc);
    urbi_vm_destroy(&vm);
}

void test_regexp_suite(void);
void test_regexp_suite(void)
{
    printf("test_regexp\n");
    utest_run("re_literal_match",          re_literal_match);
    utest_run("re_literal_substring",      re_literal_substring);
    utest_run("re_literal_no_match",       re_literal_no_match);
    utest_run("re_dot_any",                re_dot_any);
    utest_run("re_dot_no_match_empty",     re_dot_no_match_empty);
    utest_run("re_anchor_start",           re_anchor_start);
    utest_run("re_anchor_start_fail",      re_anchor_start_fail);
    utest_run("re_anchor_end",             re_anchor_end);
    utest_run("re_anchor_end_fail",        re_anchor_end_fail);
    utest_run("re_anchor_both",            re_anchor_both);
    utest_run("re_anchor_both_fail",       re_anchor_both_fail);
    utest_run("re_empty_matches",          re_empty_matches);
    utest_run("re_star_zero",              re_star_zero);
    utest_run("re_star_many",              re_star_many);
    utest_run("re_plus_one",              re_plus_one);
    utest_run("re_plus_zero_fail",         re_plus_zero_fail);
    utest_run("re_question_present",       re_question_present);
    utest_run("re_question_absent",        re_question_absent);
    utest_run("re_dotstar",                re_dotstar);
    utest_run("re_greedy_backtrack",       re_greedy_backtrack);
    utest_run("re_class_member",           re_class_member);
    utest_run("re_class_range",            re_class_range);
    utest_run("re_class_range_fail",       re_class_range_fail);
    utest_run("re_class_negated",          re_class_negated);
    utest_run("re_class_negated_fail",     re_class_negated_fail);
    utest_run("re_class_anchored",         re_class_anchored);
    utest_run("re_class_anchored_fail",    re_class_anchored_fail);
    utest_run("re_e2e_test_true",          re_e2e_test_true);
    utest_run("re_e2e_test_false",         re_e2e_test_false);
    utest_run("re_e2e_match_alias",        re_e2e_match_alias);
    utest_run("re_budget_steps_exhausted", re_budget_steps_exhausted);
    utest_run("re_budget_depth_exhausted", re_budget_depth_exhausted);
    utest_run("re_e2e_budget_raises",      re_e2e_budget_raises);
}
