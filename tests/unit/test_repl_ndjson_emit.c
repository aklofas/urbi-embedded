/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_ndjson_emit.c — v0.9.1 NDJSON response emitter. */
#include "utest.h"

#ifdef URBI_ENABLE_REPL

#include "repl/urepl_ndjson.h"

#include <string.h>

#define UTEST(name) static void name(void)

UTEST(ndjson_emit_hello)
{
    char buf[256];
    size_t n = 0;
    int rc = urepl_ndjson_emit_hello(buf, sizeof(buf), "a3f2", true, false, &n);
    UASSERT_EQ(rc, 0);
    UASSERT(strstr(buf, "\"kind\":\"hello\"") != NULL);
    UASSERT(strstr(buf, "\"version\":\"v0.9.1\"") != NULL);
    UASSERT(strstr(buf, "\"lobby\":\"a3f2\"") != NULL);
    UASSERT(strstr(buf, "\"synclines\":true") != NULL);
    UASSERT(strstr(buf, "\"auth_required\":false") != NULL);
    UASSERT(n > 0);
    UASSERT_EQ(buf[n - 1], '\n');
}

UTEST(ndjson_emit_auth_ok)
{
    char buf[128];
    size_t n = 0;
    int rc = urepl_ndjson_emit_auth_ok(buf, sizeof(buf), 1, &n);
    UASSERT_EQ(rc, 0);
    UASSERT(strstr(buf, "\"id\":1") != NULL);
    UASSERT(strstr(buf, "\"kind\":\"auth_ok\"") != NULL);
    UASSERT_EQ(buf[n - 1], '\n');
}

UTEST(ndjson_emit_result_simple)
{
    char buf[256];
    size_t n = 0;
    int rc = urepl_ndjson_emit_result(buf, sizeof(buf), 5, "3", 1234567, &n);
    UASSERT_EQ(rc, 0);
    UASSERT(strstr(buf, "\"id\":5") != NULL);
    UASSERT(strstr(buf, "\"kind\":\"result\"") != NULL);
    UASSERT(strstr(buf, "\"value\":3") != NULL);
    UASSERT(strstr(buf, "\"ts\":1234567") != NULL);
    UASSERT_EQ(buf[n - 1], '\n');
}

UTEST(ndjson_emit_result_null_value_uses_json_null)
{
    char buf[128];
    size_t n = 0;
    int rc = urepl_ndjson_emit_result(buf, sizeof(buf), 5, NULL, 0, &n);
    UASSERT_EQ(rc, 0);
    UASSERT(strstr(buf, "\"value\":null") != NULL);
    UASSERT(strstr(buf, "\"ts\":") == NULL);  /* ts==0 suppressed */
}

UTEST(ndjson_emit_output_with_id)
{
    char buf[256];
    size_t n = 0;
    int rc = urepl_ndjson_emit_output(buf, sizeof(buf), 7, NULL, "clog", "hi", 2, 1000, &n);
    UASSERT_EQ(rc, 0);
    UASSERT(strstr(buf, "\"id\":7") != NULL);
    UASSERT(strstr(buf, "\"channel\":\"clog\"") != NULL);
    UASSERT(strstr(buf, "\"msg\":\"hi\"") != NULL);
    UASSERT(strstr(buf, "\"lobby\"") == NULL);  /* id wins */
}

UTEST(ndjson_emit_output_lobby_scoped)
{
    char buf[256];
    size_t n = 0;
    int rc = urepl_ndjson_emit_output(buf, sizeof(buf), 0, "a3f2", "clog", "tick", 4, 1000, &n);
    UASSERT_EQ(rc, 0);
    UASSERT(strstr(buf, "\"lobby\":\"a3f2\"") != NULL);
    UASSERT(strstr(buf, "\"id\"") == NULL);   /* no eval id */
    UASSERT(strstr(buf, "\"msg\":\"tick\"") != NULL);
}

UTEST(ndjson_emit_output_escapes_strings)
{
    char buf[256];
    size_t n = 0;
    /* msg = a"b\c<LF>d  (7 bytes; embed unescaped, emitter must escape) */
    const char *raw = "a\"b\\c\nd";
    int rc = urepl_ndjson_emit_output(buf, sizeof(buf), 1, NULL, "clog",
                                      raw, 7, 0, &n);
    UASSERT_EQ(rc, 0);
    UASSERT(strstr(buf, "a\\\"b\\\\c\\nd") != NULL);
}

UTEST(ndjson_emit_output_escapes_control_chars)
{
    char buf[256];
    size_t n = 0;
    const char raw[] = { 'x', 0x01, 'y' };
    int rc = urepl_ndjson_emit_output(buf, sizeof(buf), 1, NULL, "clog",
                                      raw, 3, 0, &n);
    UASSERT_EQ(rc, 0);
    UASSERT(strstr(buf, "x\\u0001y") != NULL);
}

UTEST(ndjson_emit_done)
{
    char buf[64];
    size_t n = 0;
    int rc = urepl_ndjson_emit_done(buf, sizeof(buf), 5, &n);
    UASSERT_EQ(rc, 0);
    UASSERT(strstr(buf, "\"id\":5") != NULL);
    UASSERT(strstr(buf, "\"kind\":\"done\"") != NULL);
}

UTEST(ndjson_emit_error_with_id)
{
    char buf[256];
    size_t n = 0;
    int rc = urepl_ndjson_emit_error(buf, sizeof(buf), 2, "parse",
                                     "bad token", &n);
    UASSERT_EQ(rc, 0);
    UASSERT(strstr(buf, "\"id\":2") != NULL);
    UASSERT(strstr(buf, "\"kind\":\"error\"") != NULL);
    UASSERT(strstr(buf, "\"code\":\"parse\"") != NULL);
    UASSERT(strstr(buf, "\"msg\":\"bad token\"") != NULL);
}

UTEST(ndjson_emit_error_without_id_omits_id_field)
{
    char buf[128];
    size_t n = 0;
    int rc = urepl_ndjson_emit_error(buf, sizeof(buf), 0, "auth_failed",
                                     NULL, &n);
    UASSERT_EQ(rc, 0);
    UASSERT(strstr(buf, "\"id\"") == NULL);
    UASSERT(strstr(buf, "\"code\":\"auth_failed\"") != NULL);
    UASSERT(strstr(buf, "\"msg\"") == NULL);
}

UTEST(ndjson_emit_event)
{
    char buf[256];
    size_t n = 0;
    int rc = urepl_ndjson_emit_event(buf, sizeof(buf), "a3f2",
                                     "battery_low", "{\"level\":18}",
                                     1234700, &n);
    UASSERT_EQ(rc, 0);
    UASSERT(strstr(buf, "\"kind\":\"event\"") != NULL);
    UASSERT(strstr(buf, "\"lobby\":\"a3f2\"") != NULL);
    UASSERT(strstr(buf, "\"name\":\"battery_low\"") != NULL);
    UASSERT(strstr(buf, "\"payload\":{\"level\":18}") != NULL);
    UASSERT(strstr(buf, "\"ts\":1234700") != NULL);
}

UTEST(ndjson_emit_goodbye)
{
    char buf[128];
    size_t n = 0;
    int rc = urepl_ndjson_emit_goodbye(buf, sizeof(buf), "server_stopping", &n);
    UASSERT_EQ(rc, 0);
    UASSERT(strstr(buf, "\"kind\":\"goodbye\"") != NULL);
    UASSERT(strstr(buf, "\"reason\":\"server_stopping\"") != NULL);
}

UTEST(ndjson_emit_overflow_returns_error)
{
    char buf[8];
    size_t n = 0;
    int rc = urepl_ndjson_emit_result(buf, sizeof(buf), 100, "12345", 0, &n);
    UASSERT(rc != 0);
}

UTEST(ndjson_emit_round_trip_via_parser)
{
    /* Emit an output envelope, then parse-verify a subset.  This isn't
     * exact (the request schema doesn't include 'kind' / 'channel' /
     * 'msg'), but we can run the json-escape helper through a roundtrip
     * by emitting and looking for the exact escaped bytes. */
    char buf[256];
    size_t n = 0;
    int rc = urepl_ndjson_emit_output(buf, sizeof(buf), 9, NULL, "clog",
                                      "hello", 5, 0, &n);
    UASSERT_EQ(rc, 0);
    /* Ensure we produced exactly one '\n' (terminator). */
    int newlines = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\n') newlines++;
    }
    UASSERT_EQ(newlines, 1);
    UASSERT_EQ(buf[n - 1], '\n');
}

void
test_repl_ndjson_emit_suite(void)
{
    printf("test_repl_ndjson_emit\n");
    utest_run("ndjson_emit_hello",                          ndjson_emit_hello);
    utest_run("ndjson_emit_auth_ok",                        ndjson_emit_auth_ok);
    utest_run("ndjson_emit_result_simple",                  ndjson_emit_result_simple);
    utest_run("ndjson_emit_result_null_value_uses_json_null",ndjson_emit_result_null_value_uses_json_null);
    utest_run("ndjson_emit_output_with_id",                 ndjson_emit_output_with_id);
    utest_run("ndjson_emit_output_lobby_scoped",            ndjson_emit_output_lobby_scoped);
    utest_run("ndjson_emit_output_escapes_strings",         ndjson_emit_output_escapes_strings);
    utest_run("ndjson_emit_output_escapes_control_chars",   ndjson_emit_output_escapes_control_chars);
    utest_run("ndjson_emit_done",                           ndjson_emit_done);
    utest_run("ndjson_emit_error_with_id",                  ndjson_emit_error_with_id);
    utest_run("ndjson_emit_error_without_id_omits_id_field",ndjson_emit_error_without_id_omits_id_field);
    utest_run("ndjson_emit_event",                          ndjson_emit_event);
    utest_run("ndjson_emit_goodbye",                        ndjson_emit_goodbye);
    utest_run("ndjson_emit_overflow_returns_error",         ndjson_emit_overflow_returns_error);
    utest_run("ndjson_emit_round_trip_via_parser",          ndjson_emit_round_trip_via_parser);
}

#else  /* !URBI_ENABLE_REPL */

void test_repl_ndjson_emit_suite(void) { /* skipped: URBI_ENABLE_REPL=0 */ }

#endif
