/* SPDX-License-Identifier: BSD-3-Clause */
/* test_regexp.c — RegExp matcher + type wiring (v1.0 stdlib-completeness).
 *
 * Exercises the compact backtracking matcher directly (urbi_regexp_search)
 * across literals, `.`, `*`, `+`, `?`, `^`, `$`, and char classes, plus the
 * end-to-end RegExp.new / .test / .match surface. */

#include "utest.h"
#include "stdlib/regexp.h"
#include "utest_e2e_helpers.h"
#include "vm/uvm.h"
#include "urbi/urbi.h"
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

void test_regexp_suite(void);
void test_regexp_suite(void)
{
    printf("test_regexp\n");
    utest_run("re_literal_match",        re_literal_match);
    utest_run("re_literal_substring",    re_literal_substring);
    utest_run("re_literal_no_match",     re_literal_no_match);
    utest_run("re_dot_any",              re_dot_any);
    utest_run("re_dot_no_match_empty",   re_dot_no_match_empty);
    utest_run("re_anchor_start",         re_anchor_start);
    utest_run("re_anchor_start_fail",    re_anchor_start_fail);
    utest_run("re_anchor_end",           re_anchor_end);
    utest_run("re_anchor_end_fail",      re_anchor_end_fail);
    utest_run("re_anchor_both",          re_anchor_both);
    utest_run("re_anchor_both_fail",     re_anchor_both_fail);
    utest_run("re_empty_matches",        re_empty_matches);
    utest_run("re_star_zero",            re_star_zero);
    utest_run("re_star_many",            re_star_many);
    utest_run("re_plus_one",             re_plus_one);
    utest_run("re_plus_zero_fail",       re_plus_zero_fail);
    utest_run("re_question_present",     re_question_present);
    utest_run("re_question_absent",      re_question_absent);
    utest_run("re_dotstar",              re_dotstar);
    utest_run("re_greedy_backtrack",     re_greedy_backtrack);
    utest_run("re_class_member",         re_class_member);
    utest_run("re_class_range",          re_class_range);
    utest_run("re_class_range_fail",     re_class_range_fail);
    utest_run("re_class_negated",        re_class_negated);
    utest_run("re_class_negated_fail",   re_class_negated_fail);
    utest_run("re_class_anchored",       re_class_anchored);
    utest_run("re_class_anchored_fail",  re_class_anchored_fail);
    utest_run("re_e2e_test_true",        re_e2e_test_true);
    utest_run("re_e2e_test_false",       re_e2e_test_false);
    utest_run("re_e2e_match_alias",      re_e2e_match_alias);
}
