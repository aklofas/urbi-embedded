/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_ndjson_parse.c — v0.9.1 NDJSON request parser.
 *
 * Only built when URBI_ENABLE_REPL=1 (the gated TU under
 * src/repl/urepl_ndjson.c is otherwise absent).  The suite-registration
 * call in runner.c is itself gated by URBI_ENABLE_REPL so the symbol
 * does not need to exist in default builds. */
#include "utest.h"

#ifdef URBI_ENABLE_REPL

#include "repl/urepl_ndjson.h"

#include <string.h>

#define UTEST(name) static void name(void)

UTEST(ndjson_parse_eval_op)
{
    const char *line = "{\"id\":5,\"op\":\"eval\",\"code\":\"1+2\"}";
    UReplNdjsonReq req;
    int rc = urepl_ndjson_parse(line, strlen(line), &req);
    UASSERT_EQ(rc, 0);
    UASSERT_EQ(req.id, 5);
    UASSERT_EQ(req.op, UREPL_OP_EVAL);
    UASSERT(req.code != NULL);
    UASSERT_EQ(req.code_len, 3);
    UASSERT(memcmp(req.code, "1+2", 3) == 0);
    urepl_ndjson_free_req(&req);
}

UTEST(ndjson_parse_auth_op)
{
    const char *line = "{\"id\":1,\"op\":\"auth\",\"token\":\"sekret\"}";
    UReplNdjsonReq req;
    int rc = urepl_ndjson_parse(line, strlen(line), &req);
    UASSERT_EQ(rc, 0);
    UASSERT_EQ(req.op, UREPL_OP_AUTH);
    UASSERT(req.token != NULL);
    UASSERT(strcmp(req.token, "sekret") == 0);
    urepl_ndjson_free_req(&req);
}

UTEST(ndjson_parse_introspect_op)
{
    const char *line = "{\"id\":3,\"op\":\"introspect\",\"what\":\"coros\"}";
    UReplNdjsonReq req;
    int rc = urepl_ndjson_parse(line, strlen(line), &req);
    UASSERT_EQ(rc, 0);
    UASSERT_EQ(req.op, UREPL_OP_INTROSPECT);
    UASSERT(req.what != NULL);
    UASSERT(strcmp(req.what, "coros") == 0);
    urepl_ndjson_free_req(&req);
}

UTEST(ndjson_parse_cancel_with_tag)
{
    const char *line = "{\"id\":6,\"op\":\"cancel\",\"lobby\":\"a3f2\",\"tag\":\"exp_42\"}";
    UReplNdjsonReq req;
    int rc = urepl_ndjson_parse(line, strlen(line), &req);
    UASSERT_EQ(rc, 0);
    UASSERT_EQ(req.op, UREPL_OP_CANCEL);
    UASSERT(req.lobby != NULL && strcmp(req.lobby, "a3f2") == 0);
    UASSERT(req.tag != NULL && strcmp(req.tag, "exp_42") == 0);
    urepl_ndjson_free_req(&req);
}

UTEST(ndjson_parse_lobby_new)
{
    const char *line = "{\"id\":7,\"op\":\"lobby_new\"}";
    UReplNdjsonReq req;
    int rc = urepl_ndjson_parse(line, strlen(line), &req);
    UASSERT_EQ(rc, 0);
    UASSERT_EQ(req.op, UREPL_OP_LOBBY_NEW);
    urepl_ndjson_free_req(&req);
}

UTEST(ndjson_parse_lobby_close)
{
    const char *line = "{\"id\":8,\"op\":\"lobby_close\",\"lobby\":\"a3f2\"}";
    UReplNdjsonReq req;
    int rc = urepl_ndjson_parse(line, strlen(line), &req);
    UASSERT_EQ(rc, 0);
    UASSERT_EQ(req.op, UREPL_OP_LOBBY_CLOSE);
    UASSERT(req.lobby != NULL && strcmp(req.lobby, "a3f2") == 0);
    urepl_ndjson_free_req(&req);
}

UTEST(ndjson_parse_eval_with_synclines)
{
    const char *line =
        "{\"id\":2,\"op\":\"eval\",\"file\":\"/p/b.u\",\"line\":42,\"code\":\"x\"}";
    UReplNdjsonReq req;
    int rc = urepl_ndjson_parse(line, strlen(line), &req);
    UASSERT_EQ(rc, 0);
    UASSERT_EQ(req.line, 42);
    UASSERT(req.file != NULL && strcmp(req.file, "/p/b.u") == 0);
    UASSERT(req.code != NULL && strcmp(req.code, "x") == 0);
    urepl_ndjson_free_req(&req);
}

UTEST(ndjson_parse_introspect_stack_with_coro_id)
{
    const char *line = "{\"id\":4,\"op\":\"introspect\",\"what\":\"stack\",\"coro_id\":42}";
    UReplNdjsonReq req;
    int rc = urepl_ndjson_parse(line, strlen(line), &req);
    UASSERT_EQ(rc, 0);
    UASSERT_EQ(req.op, UREPL_OP_INTROSPECT);
    UASSERT_EQ(req.coro_id, 42);
    urepl_ndjson_free_req(&req);
}

UTEST(ndjson_parse_string_with_escapes)
{
    const char *line =
        "{\"id\":1,\"op\":\"eval\",\"code\":\"a\\\"b\\nc\\\\d\"}";
    UReplNdjsonReq req;
    int rc = urepl_ndjson_parse(line, strlen(line), &req);
    UASSERT_EQ(rc, 0);
    UASSERT(req.code != NULL);
    UASSERT_EQ(req.code_len, 7);
    UASSERT(memcmp(req.code, "a\"b\nc\\d", 7) == 0);
    urepl_ndjson_free_req(&req);
}

UTEST(ndjson_parse_unicode_escape)
{
    /* é = U+00E9 LATIN SMALL E WITH ACUTE => UTF-8 0xC3 0xA9 */
    const char *line = "{\"id\":1,\"op\":\"eval\",\"code\":\"\\u00e9\"}";
    UReplNdjsonReq req;
    int rc = urepl_ndjson_parse(line, strlen(line), &req);
    UASSERT_EQ(rc, 0);
    UASSERT(req.code != NULL);
    UASSERT_EQ(req.code_len, 2);
    UASSERT_EQ((unsigned char)req.code[0], 0xC3);
    UASSERT_EQ((unsigned char)req.code[1], 0xA9);
    urepl_ndjson_free_req(&req);
}

UTEST(ndjson_parse_malformed_unterminated_rejects)
{
    const char *line = "{\"id\":5,\"op\":\"eval\"";
    UReplNdjsonReq req;
    int rc = urepl_ndjson_parse(line, strlen(line), &req);
    UASSERT(rc != 0);
}

UTEST(ndjson_parse_malformed_empty_object_rejects)
{
    const char *line = "{}";
    UReplNdjsonReq req;
    int rc = urepl_ndjson_parse(line, strlen(line), &req);
    UASSERT(rc != 0);
}

UTEST(ndjson_parse_malformed_missing_brace_rejects)
{
    const char *line = "\"id\":1,\"op\":\"auth\"";
    UReplNdjsonReq req;
    int rc = urepl_ndjson_parse(line, strlen(line), &req);
    UASSERT(rc != 0);
}

UTEST(ndjson_parse_unknown_op_rejects)
{
    const char *line = "{\"id\":1,\"op\":\"frobnicate\"}";
    UReplNdjsonReq req;
    int rc = urepl_ndjson_parse(line, strlen(line), &req);
    UASSERT(rc != 0);
}

UTEST(ndjson_parse_unknown_key_is_skipped)
{
    /* Unknown keys must be skipped silently for forward compat. */
    const char *line =
        "{\"id\":1,\"op\":\"eval\",\"extra\":{\"nested\":[1,2,3]},\"code\":\"x\"}";
    UReplNdjsonReq req;
    int rc = urepl_ndjson_parse(line, strlen(line), &req);
    UASSERT_EQ(rc, 0);
    UASSERT_EQ(req.op, UREPL_OP_EVAL);
    UASSERT(req.code != NULL && strcmp(req.code, "x") == 0);
    urepl_ndjson_free_req(&req);
}

UTEST(ndjson_parse_id_overflow_rejects)
{
    /* 99999999999999999999 > UINT64_MAX */
    const char *line = "{\"id\":99999999999999999999,\"op\":\"auth\"}";
    UReplNdjsonReq req;
    int rc = urepl_ndjson_parse(line, strlen(line), &req);
    UASSERT(rc != 0);
}

UTEST(ndjson_parse_huge_string_rejects)
{
    /* token field cap is UREPL_MAX_TOKEN = 256.  Build a 300-char token. */
    char line[512];
    int n = snprintf(line, sizeof(line), "{\"id\":1,\"op\":\"auth\",\"token\":\"");
    UASSERT(n > 0);
    size_t off = (size_t)n;
    for (int i = 0; i < 300; i++) {
        line[off++] = 'A';
    }
    line[off++] = '"';
    line[off++] = '}';
    int rc = urepl_ndjson_parse(line, off, NULL);
    UASSERT(rc != 0);
    UReplNdjsonReq req;
    rc = urepl_ndjson_parse(line, off, &req);
    UASSERT(rc != 0);
}

UTEST(ndjson_parse_bare_control_char_rejects)
{
    /* Unescaped \n inside a JSON string is a parse error. */
    char line[64];
    int n = snprintf(line, sizeof(line), "{\"id\":1,\"op\":\"eval\",\"code\":\"a");
    UASSERT(n > 0);
    line[n++] = '\n';
    line[n++] = '"';
    line[n++] = '}';
    UReplNdjsonReq req;
    int rc = urepl_ndjson_parse(line, (size_t)n, &req);
    UASSERT(rc != 0);
}

void
test_repl_ndjson_parse_suite(void)
{
    printf("test_repl_ndjson_parse\n");
    utest_run("ndjson_parse_eval_op",                       ndjson_parse_eval_op);
    utest_run("ndjson_parse_auth_op",                       ndjson_parse_auth_op);
    utest_run("ndjson_parse_introspect_op",                 ndjson_parse_introspect_op);
    utest_run("ndjson_parse_cancel_with_tag",               ndjson_parse_cancel_with_tag);
    utest_run("ndjson_parse_lobby_new",                     ndjson_parse_lobby_new);
    utest_run("ndjson_parse_lobby_close",                   ndjson_parse_lobby_close);
    utest_run("ndjson_parse_eval_with_synclines",           ndjson_parse_eval_with_synclines);
    utest_run("ndjson_parse_introspect_stack_with_coro_id", ndjson_parse_introspect_stack_with_coro_id);
    utest_run("ndjson_parse_string_with_escapes",           ndjson_parse_string_with_escapes);
    utest_run("ndjson_parse_unicode_escape",                ndjson_parse_unicode_escape);
    utest_run("ndjson_parse_malformed_unterminated_rejects",ndjson_parse_malformed_unterminated_rejects);
    utest_run("ndjson_parse_malformed_empty_object_rejects",ndjson_parse_malformed_empty_object_rejects);
    utest_run("ndjson_parse_malformed_missing_brace_rejects",ndjson_parse_malformed_missing_brace_rejects);
    utest_run("ndjson_parse_unknown_op_rejects",            ndjson_parse_unknown_op_rejects);
    utest_run("ndjson_parse_unknown_key_is_skipped",        ndjson_parse_unknown_key_is_skipped);
    utest_run("ndjson_parse_id_overflow_rejects",           ndjson_parse_id_overflow_rejects);
    utest_run("ndjson_parse_huge_string_rejects",           ndjson_parse_huge_string_rejects);
    utest_run("ndjson_parse_bare_control_char_rejects",     ndjson_parse_bare_control_char_rejects);
}

#else  /* !URBI_ENABLE_REPL — suite is a no-op stub for default builds. */

void test_repl_ndjson_parse_suite(void) { /* skipped: URBI_ENABLE_REPL=0 */ }

#endif

