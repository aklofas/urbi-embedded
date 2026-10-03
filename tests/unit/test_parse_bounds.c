/* SPDX-License-Identifier: BSD-3-Clause */
/* test_parse_bounds.c — parser buffer-bound + nesting-state regressions
 * (refactor-3 FE-10 + FE-22).
 *
 * FE-10: parse_assert's leading-whitespace trim loop walked the source
 * buffer with no bound.  `assert(` as the LAST bytes of a non-NUL-
 * terminated buffer (the compile entry points explicitly permit non-NUL
 * input) put p->lex->cur one past the end before the loop's first
 * dereference — a heap over-read, reliably visible only under ASan.
 * The fix bounds the loop at p->lex->end.
 *
 * FE-22: parse_at / parse_whenever / parse_waituntil set the parser
 * flag at_event_cond with absolute writes (`= true` ... `= false`).
 * parse_atom dispatches TOK_KW_WAITUNTIL as an expression primary, so
 * `waituntil(g?)` can nest INSIDE another form's condition; the inner
 * parse's `= false` then clobbered the outer flag and the outer
 * condition's trailing `?` was rejected with PARSE_QUESTION_OUTSIDE_AT.
 * The fix saves/restores the flag at all three sites.
 *
 * Both are parser bugs, so the cases drive the parser directly: a source
 * "parses" when every statement comes back without an AST_ERROR. */

#include "utest.h"
#include "util/uarena.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "parse/uast.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Parse exactly `len` bytes of `src`; true when no statement is an
 * AST_ERROR. */
static bool parses(const char *src, size_t len)
{
    ULexer lex;
    UArena arena;
    UParser p;
    ulex_init(&lex, src, len);
    uarena_init(&arena, 4096);
    uparse_init(&p, &lex, &arena);
    bool ok = true;
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) { ok = false; break; }
        uarena_reset(&arena);
    }
    uarena_destroy(&arena);
    return ok;
}

/* Parse `src` (exactly `len` bytes, NOT NUL-terminated). */
static bool parses_no_nul(const char *src, size_t len)
{
    /* malloc EXACTLY len bytes so any read past src[len-1] is a heap
     * over-read that ASan flags. */
    char *buf = (char *)malloc(len);
    UASSERT(buf != NULL);
    memcpy(buf, src, len);
    bool ok = parses(buf, len);
    free(buf);
    return ok;
}

static bool parses_src(const char *src) { return parses(src, strlen(src)); }

/* === FE-10: assert( at EOF of a non-NUL-terminated buffer ============= */

UTEST(assert_lparen_at_buffer_end_no_overread)
{
    /* After consuming `(`, p->lex->cur == one-past-end.  Pre-fix the trim
     * loop dereferenced it unconditionally (heap-buffer-overflow under
     * ASan).  Post-fix: clean parse error (unexpected EOF), no read. */
    static const char src[] = "assert(";
    UASSERT(!parses_no_nul(src, sizeof src - 1));
}

UTEST(assert_lparen_trailing_ws_at_buffer_end_no_overread)
{
    /* Whitespace after `(` up to the buffer end: pre-fix the loop walked
     * the spaces and then read one byte past the allocation. */
    static const char src[] = "assert( \t\n";
    UASSERT(!parses_no_nul(src, sizeof src - 1));
}

UTEST(assert_trim_still_works)
{
    /* Pin: the bounded trim must not change normal assert parsing. */
    static const char src[] = "var x = 1; assert(  x == 1  ); x";
    UASSERT(parses_no_nul(src, sizeof src - 1));
}

/* === FE-22: nested waituntil must not clobber at_event_cond =========== */

UTEST(at_cond_nested_waituntil_keeps_outer_flag)
{
    /* `waituntil(g?)` nested inside the at-condition: pre-fix, the inner
     * parse_waituntil's absolute `at_event_cond = false` clobbered the
     * flag parse_at set, so the OUTER trailing `?` was rejected with
     * PARSE_QUESTION_OUTSIDE_AT.  Post-fix this parses (at-event form
     * with a comparison event expression). */
    UASSERT(parses_src("var g = 1; var h = 2; at (waituntil(g?) == h?) g"));
}

UTEST(whenever_cond_nested_waituntil_keeps_outer_flag)
{
    /* Same clobber through parse_whenever's site. */
    UASSERT(parses_src("var g = 1; var h = 2; whenever (waituntil(g?) == h?) g"));
}

UTEST(waituntil_cond_nested_waituntil_keeps_outer_flag)
{
    /* Same clobber through parse_waituntil's own site (self-nesting). */
    UASSERT(parses_src("var g = 1; var h = 2; waituntil(waituntil(g?) == h?)"));
}

UTEST(question_outside_at_still_rejected)
{
    /* Pin: the save/restore must not weaken the base rule — a postfix
     * `?` outside any at/whenever/waituntil condition stays an error,
     * including AFTER a complete waituntil statement has run the
     * save/restore pair (restore lands back on false, not true). */
    UASSERT(!parses_src("var g = 1; g?"));
    UASSERT(!parses_src("var g = 1; waituntil(g?); g?"));
}

void test_parse_bounds_suite(void) {
    utest_run("assert_lparen_at_buffer_end_no_overread",
              assert_lparen_at_buffer_end_no_overread);
    utest_run("assert_lparen_trailing_ws_at_buffer_end_no_overread",
              assert_lparen_trailing_ws_at_buffer_end_no_overread);
    utest_run("assert_trim_still_works",
              assert_trim_still_works);
    utest_run("at_cond_nested_waituntil_keeps_outer_flag",
              at_cond_nested_waituntil_keeps_outer_flag);
    utest_run("whenever_cond_nested_waituntil_keeps_outer_flag",
              whenever_cond_nested_waituntil_keeps_outer_flag);
    utest_run("waituntil_cond_nested_waituntil_keeps_outer_flag",
              waituntil_cond_nested_waituntil_keeps_outer_flag);
    utest_run("question_outside_at_still_rejected",
              question_outside_at_still_rejected);
}
