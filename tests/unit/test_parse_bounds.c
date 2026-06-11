/* SPDX-License-Identifier: BSD-3-Clause */
/* test_parse_bounds.c — parser buffer-bound + nesting-state regressions
 * (refactor-3 FE-10 + FE-22).
 *
 * FE-10: parse_assert's leading-whitespace trim loop walked the source
 * buffer with no bound.  `assert(` as the LAST bytes of a non-NUL-
 * terminated buffer (urbi_compile_source explicitly permits non-NUL
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
 * The fix saves/restores the flag at all three sites. */

#include "utest.h"
#include "urbi/aux.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"

#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Compile `src` (exactly `len` bytes, NOT NUL-terminated) against a fresh
 * VM.  Returns the urbi_compile_source rc; frees any produced bytecode. */
static int compile_no_nul(const char *src, size_t len)
{
    /* malloc EXACTLY len bytes so any read past src[len-1] is a heap
     * over-read that ASan flags. */
    char *buf = (char *)malloc(len);
    UASSERT(buf != NULL);
    memcpy(buf, src, len);

    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    unsigned char *bc = NULL;
    size_t bc_len = 0;
    char err[256] = {0};
    int rc = urbi_compile_source(&vm, buf, len, "test",
                                 &bc, &bc_len, err, sizeof err);
    if (bc != NULL) free(bc);
    urbi_vm_destroy(&vm);
    free(buf);
    return rc;
}

/* === FE-10: assert( at EOF of a non-NUL-terminated buffer ============= */

UTEST(assert_lparen_at_buffer_end_no_overread)
{
    /* After consuming `(`, p->lex->cur == one-past-end.  Pre-fix the trim
     * loop dereferenced it unconditionally (heap-buffer-overflow under
     * ASan).  Post-fix: clean parse error (unexpected EOF), no read. */
    static const char src[] = "assert(";
    int rc = compile_no_nul(src, sizeof src - 1);
    UASSERT(rc != URBI_OK);
}

UTEST(assert_lparen_trailing_ws_at_buffer_end_no_overread)
{
    /* Whitespace after `(` up to the buffer end: pre-fix the loop walked
     * the spaces and then read one byte past the allocation. */
    static const char src[] = "assert( \t\n";
    int rc = compile_no_nul(src, sizeof src - 1);
    UASSERT(rc != URBI_OK);
}

UTEST(assert_trim_still_works)
{
    /* Pin: the bounded trim must not change normal assert parsing —
     * a passing assert with padded source text compiles and runs. */
    static const char src[] = "var x = 1; assert(  x == 1  ); x";

    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    unsigned char *bc = NULL;
    size_t bc_len = 0;
    char err[256] = {0};
    int rc = urbi_compile_source(&vm, src, sizeof src - 1, "test",
                                 &bc, &bc_len, err, sizeof err);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT(bc != NULL);

    UValue result = urbi_make_nil();
    rc = urbi_aux_load_and_run(&vm, bc, bc_len, &result);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)result.kind);
    UASSERT_EQ(1LL, (long long)result.v.i);

    free(bc);
    urbi_vm_destroy(&vm);
}

/* === FE-22: nested waituntil must not clobber at_event_cond =========== */

/* Compile a NUL-terminated source; return the rc. */
static int compile_src(const char *src)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    unsigned char *bc = NULL;
    size_t bc_len = 0;
    char err[256] = {0};
    int rc = urbi_compile_source(&vm, src, strlen(src), "test",
                                 &bc, &bc_len, err, sizeof err);
    if (bc != NULL) free(bc);
    urbi_vm_destroy(&vm);
    return rc;
}

UTEST(at_cond_nested_waituntil_keeps_outer_flag)
{
    /* `waituntil(g?)` nested inside the at-condition: pre-fix, the inner
     * parse_waituntil's absolute `at_event_cond = false` clobbered the
     * flag parse_at set, so the OUTER trailing `?` was rejected with
     * PARSE_QUESTION_OUTSIDE_AT.  Post-fix this parses (at-event form
     * with a comparison event expression) and compiles. */
    int rc = compile_src(
        "var g = 1; var h = 2; at (waituntil(g?) == h?) g");
    UASSERT_EQ(URBI_OK, rc);
}

UTEST(whenever_cond_nested_waituntil_keeps_outer_flag)
{
    /* Same clobber through parse_whenever's site. */
    int rc = compile_src(
        "var g = 1; var h = 2; whenever (waituntil(g?) == h?) g");
    UASSERT_EQ(URBI_OK, rc);
}

UTEST(waituntil_cond_nested_waituntil_keeps_outer_flag)
{
    /* Same clobber through parse_waituntil's own site (self-nesting). */
    int rc = compile_src(
        "var g = 1; var h = 2; waituntil(waituntil(g?) == h?)");
    UASSERT_EQ(URBI_OK, rc);
}

UTEST(question_outside_at_still_rejected)
{
    /* Pin: the save/restore must not weaken the base rule — a postfix
     * `?` outside any at/whenever/waituntil condition stays an error,
     * including AFTER a complete waituntil statement has run the
     * save/restore pair (restore lands back on false, not true). */
    UASSERT(compile_src("var g = 1; g?") != URBI_OK);
    UASSERT(compile_src("var g = 1; waituntil(g?); g?") != URBI_OK);
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
