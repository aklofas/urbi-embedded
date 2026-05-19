/* SPDX-License-Identifier: BSD-3-Clause */
/* Tests for //#line / //#push / //#pop syncline directives in the lexer.
 * v0.9.0-repl. */

#include "utest.h"

#include <string.h>

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "lex/ulex.h"
#include "runtime/umacros.h"   /* urbi_zero */

#define UTEST(name) static void name(void)

/* Helper: advance the lexer past one token, returning it. */
static UToken next_tok(ULexer *l)
{
    return ulex_next(l);
}

/* Helper: consume tokens until we see the given type, then return it.
 * Stops at TOK_EOF regardless.  Useful for skipping to a known landmark. */
static UToken consume_to(ULexer *l, UTokenType t)
{
    UToken tok;
    do {
        tok = ulex_next(l);
    } while (tok.type != t && tok.type != TOK_EOF);
    return tok;
}

/* -----------------------------------------------------------------------
 * Test 1: //#line sets line number and source_name for the next token
 * ----------------------------------------------------------------------- */

UTEST(syncline_line_directive)
{
    const char *src = "//#line 100 \"foo.u\"\nvar x = 1";
    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    UToken tok = next_tok(&lex);

    UASSERT_EQ(TOK_KW_VAR, tok.type);
    UASSERT_EQ(100, tok.line);
    UASSERT_STR_EQ("foo.u", ulex_current_source(&lex));
}

/* -----------------------------------------------------------------------
 * Test 2: //#push saves state; //#pop restores it
 * ----------------------------------------------------------------------- */

UTEST(syncline_push_pop)
{
    const char *src =
        "var a = 1;\n"
        "//#push 50 \"inner.u\"\n"
        "var b = 2;\n"
        "//#pop\n"
        "var c = 3;\n";

    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    /* "var a" at line 1, source "<stdin>". */
    UToken tok = next_tok(&lex);
    UASSERT_EQ(TOK_KW_VAR, tok.type);
    UASSERT_EQ(1, tok.line);
    UASSERT_STR_EQ("<stdin>", ulex_current_source(&lex));

    /* Consume to ';' at end of first statement. */
    consume_to(&lex, TOK_SEMI);

    /* "var b" should be at line 50, source "inner.u". */
    tok = next_tok(&lex);
    UASSERT_EQ(TOK_KW_VAR, tok.type);
    UASSERT_EQ(50, tok.line);
    UASSERT_STR_EQ("inner.u", ulex_current_source(&lex));

    /* Consume to ';' at end of second statement. */
    consume_to(&lex, TOK_SEMI);

    /* After //#pop, source_name restored to "<stdin>". */
    tok = next_tok(&lex);
    UASSERT_EQ(TOK_KW_VAR, tok.type);
    UASSERT_STR_EQ("<stdin>", ulex_current_source(&lex));
}

/* -----------------------------------------------------------------------
 * Test 3: overflow — 5th push on a depth-4 stack silently drops
 * ----------------------------------------------------------------------- */

UTEST(syncline_overflow_degrades)
{
    /* Push 5 times; cap is URBI_SYNCLINE_STACK_MAX == 4.
     * The 5th push is silently dropped, but N + FILE are still applied
     * (same as //#line).  After all 5, source_name should be "e" (the 5th
     * push's filename was applied as a plain //#line even though no slot
     * was consumed on the full stack). */
    const char *src =
        "//#push 10 \"a\"\n"
        "//#push 20 \"b\"\n"
        "//#push 30 \"c\"\n"
        "//#push 40 \"d\"\n"
        "//#push 50 \"e\"\n"
        "var x = 1\n";

    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    UToken tok = next_tok(&lex);
    UASSERT_EQ(TOK_KW_VAR, tok.type);
    /* The 5th push applied N+FILE even though overflow; source is "e". */
    UASSERT_STR_EQ("e", ulex_current_source(&lex));
}

/* -----------------------------------------------------------------------
 * Test 4: underflow — //#pop on empty stack is a silent no-op
 * ----------------------------------------------------------------------- */

UTEST(syncline_underflow_degrades)
{
    const char *src = "//#pop\nvar x = 1\n";
    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    UToken tok = next_tok(&lex);
    UASSERT_EQ(TOK_KW_VAR, tok.type);
    /* source_name unchanged — still "<stdin>". */
    UASSERT_STR_EQ("<stdin>", ulex_current_source(&lex));
}

/* -----------------------------------------------------------------------
 * Test 5: malformed //#line (non-numeric line number) falls through as comment
 * ----------------------------------------------------------------------- */

UTEST(syncline_malformed_is_comment)
{
    const char *src = "//#line abc \"foo.u\"\nvar x = 1\n";
    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    UToken tok = next_tok(&lex);
    UASSERT_EQ(TOK_KW_VAR, tok.type);
    /* Malformed directive treated as plain comment — source unchanged. */
    UASSERT_STR_EQ("<stdin>", ulex_current_source(&lex));
}

/* -----------------------------------------------------------------------
 * Test 6: unrecognized directive name falls through as plain comment
 * ----------------------------------------------------------------------- */

UTEST(syncline_unrecognized_directive_is_comment)
{
    const char *src = "//#frobnicate gizmo\nvar x = 1\n";
    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    UToken tok = next_tok(&lex);
    UASSERT_EQ(TOK_KW_VAR, tok.type);
    /* Unknown directive treated as plain comment — source unchanged. */
    UASSERT_STR_EQ("<stdin>", ulex_current_source(&lex));
}

/* -----------------------------------------------------------------------
 * Test 7: error messages inside syncline framing show the claimed source name
 * ----------------------------------------------------------------------- */

UTEST(syncline_error_message_uses_source_name)
{
    UVM vm;
    urbi_zero(&vm, sizeof vm);
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    /* Frame a parse error inside //#push "test.u"; the error message must
     * say "test.u" not "<stdin>". */
    const char *src =
        "//#push 5 \"test.u\"\n"
        "var x = $$$ |\n"        /* invalid syntax → parse error */
        "//#pop\n";
    char buf[512] = {0};
    int rc = urbi_repl_eval(&vm, realm, src, strlen(src), buf, sizeof buf);

    UASSERT(rc != URBI_OK);
    UASSERT(strstr(buf, "test.u") != NULL);
    UASSERT(strstr(buf, "<stdin>") == NULL);

    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Suite entry
 * ----------------------------------------------------------------------- */

void
test_lexer_syncline_suite(void)
{
    utest_run("syncline: //#line sets line + source_name",
              syncline_line_directive);
    utest_run("syncline: //#push saves state; //#pop restores",
              syncline_push_pop);
    utest_run("syncline: overflow on full stack degrades silently",
              syncline_overflow_degrades);
    utest_run("syncline: underflow on empty stack is no-op",
              syncline_underflow_degrades);
    utest_run("syncline: malformed directive falls through as comment",
              syncline_malformed_is_comment);
    utest_run("syncline: unrecognized directive falls through as comment",
              syncline_unrecognized_directive_is_comment);
    utest_run("syncline: error message uses source name from framing",
              syncline_error_message_uses_source_name);
}
