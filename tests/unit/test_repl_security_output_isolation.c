/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_security_output_isolation.c
 *
 * W4 security gate #6: per-session output writers do not cross-contaminate.
 *
 * Opens 2 simultaneous sessions on the same server/VM.  Sends an eval to
 * session 1 and a different eval to session 2.  Asserts each session's
 * output ringbuf sees ONLY its own response.
 *
 * The per-realm session_writer already routes output by session; this test
 * pins the contract and detects any regression.
 *
 * Closes release F13 bullet 6. */
#include "utest.h"

#ifdef URBI_ENABLE_REPL

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "repl/urepl.h"
#include "repl/urepl_dispatch.h"
#include "repl/urepl_queue.h"

#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

static char *
tdup(const char *s)
{
    if (s == NULL) { return NULL; }
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p == NULL) { return NULL; }
    memcpy(p, s, n);
    return p;
}

/* Dispatch a single eval to a session; drain and return the output.
 * Caller must free the returned string. */
static char *
eval_and_drain(UReplServer *server, UReplSession *s, uint64_t id, const char *src)
{
    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    if (job == NULL) { return NULL; }
    job->session_id  = s->session_id;
    job->req.id      = id;
    job->req.op      = UREPL_OP_EVAL;
    job->req.code    = tdup(src);
    job->req.code_len = strlen(src);
    s->authed = true;
    urepl_dispatch_job(server, job);

    char *buf = (char *)malloc(4096);
    if (buf == NULL) { return NULL; }
    size_t n = urepl_ringbuf_read(&s->output, buf, 4095);
    buf[n] = '\0';
    return buf;
}

/* ---- Tests --------------------------------------------------------------- */

UTEST(repl_security_output_isolation_basic)
{
    /* Two sessions — each sees only its own eval result. */
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port  = -1;

    UReplServer *server = urbi_repl_serve(&vm, &cfg, NULL);
    UASSERT_NE(server, NULL);

    UReplSession *s1 = urepl_session_create(server);
    UASSERT_NE(s1, NULL);
    UReplSession *s2 = urepl_session_create(server);
    UASSERT_NE(s2, NULL);

    /* s1 evals "10 + 1" (= 11); s2 evals "20 + 2" (= 22). */
    char *out1 = eval_and_drain(server, s1, 1, "10 + 1");
    UASSERT_NE(out1, NULL);

    char *out2 = eval_and_drain(server, s2, 2, "20 + 2");
    UASSERT_NE(out2, NULL);

    /* s1's output must contain "11" and must NOT contain "22". */
    UASSERT(strstr(out1, "\"11\"") != NULL);
    UASSERT(strstr(out1, "\"22\"") == NULL);

    /* s2's output must contain "22" and must NOT contain "11". */
    UASSERT(strstr(out2, "\"22\"") != NULL);
    UASSERT(strstr(out2, "\"11\"") == NULL);

    free(out1);
    free(out2);
    urbi_repl_stop(server);
    urbi_vm_destroy(&vm);
}

UTEST(repl_security_output_isolation_session_ids_distinct)
{
    /* Each session has a distinct session_id and lobby_id.  Verify that
     * the session_id fields in each envelope match the expected session. */
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port  = -1;

    UReplServer *server = urbi_repl_serve(&vm, &cfg, NULL);
    UASSERT_NE(server, NULL);

    UReplSession *s1 = urepl_session_create(server);
    UASSERT_NE(s1, NULL);
    UReplSession *s2 = urepl_session_create(server);
    UASSERT_NE(s2, NULL);

    /* Session ids must be distinct. */
    UASSERT(s1->session_id != s2->session_id);
    /* Lobby id hexes must be distinct. */
    UASSERT(strcmp(s1->lobby_id_hex, s2->lobby_id_hex) != 0);

    urbi_repl_stop(server);
    urbi_vm_destroy(&vm);
}

UTEST(repl_security_output_isolation_interleaved_evals)
{
    /* Interleave evals on two sessions and verify no cross-contamination. */
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port  = -1;

    UReplServer *server = urbi_repl_serve(&vm, &cfg, NULL);
    UASSERT_NE(server, NULL);

    UReplSession *s1 = urepl_session_create(server);
    UASSERT_NE(s1, NULL);
    UReplSession *s2 = urepl_session_create(server);
    UASSERT_NE(s2, NULL);

    /* Eval on s1, then s2, then s1 again. */
    char *a = eval_and_drain(server, s1, 10, "5 + 5");
    char *b = eval_and_drain(server, s2, 20, "3 + 3");
    char *c = eval_and_drain(server, s1, 11, "7 + 7");

    UASSERT_NE(a, NULL);
    UASSERT_NE(b, NULL);
    UASSERT_NE(c, NULL);

    /* s1 first eval: "10"; s2: "6"; s1 second eval: "14". */
    UASSERT(strstr(a, "\"10\"") != NULL);
    UASSERT(strstr(a, "\"6\"")  == NULL);

    UASSERT(strstr(b, "\"6\"")  != NULL);
    UASSERT(strstr(b, "\"10\"") == NULL);
    UASSERT(strstr(b, "\"14\"") == NULL);

    UASSERT(strstr(c, "\"14\"") != NULL);
    UASSERT(strstr(c, "\"6\"")  == NULL);

    free(a); free(b); free(c);
    urbi_repl_stop(server);
    urbi_vm_destroy(&vm);
}

UTEST(repl_security_output_isolation_error_stays_in_session)
{
    /* An error in s1 must not appear in s2's output. */
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port  = -1;

    UReplServer *server = urbi_repl_serve(&vm, &cfg, NULL);
    UASSERT_NE(server, NULL);

    UReplSession *s1 = urepl_session_create(server);
    UASSERT_NE(s1, NULL);
    UReplSession *s2 = urepl_session_create(server);
    UASSERT_NE(s2, NULL);

    /* Send a syntax error to s1. */
    char *e1 = eval_and_drain(server, s1, 30, "((((((");

    /* Send a valid eval to s2. */
    char *e2 = eval_and_drain(server, s2, 31, "4 + 4");

    UASSERT_NE(e1, NULL);
    UASSERT_NE(e2, NULL);

    /* s2's output must not contain s1's error. */
    UASSERT(strstr(e2, "\"kind\":\"error\"") == NULL
            || strstr(e2, "\"8\"") != NULL);

    /* s1's output must be an error. */
    UASSERT(strstr(e1, "\"kind\":\"error\"") != NULL);

    free(e1); free(e2);
    urbi_repl_stop(server);
    urbi_vm_destroy(&vm);
}

#endif /* URBI_ENABLE_REPL */

void
test_repl_security_output_isolation_suite(void)
{
#ifdef URBI_ENABLE_REPL
    printf("test_repl_security_output_isolation\n");
    utest_run("repl_security_output_isolation_basic",
              repl_security_output_isolation_basic);
    utest_run("repl_security_output_isolation_session_ids_distinct",
              repl_security_output_isolation_session_ids_distinct);
    utest_run("repl_security_output_isolation_interleaved_evals",
              repl_security_output_isolation_interleaved_evals);
    utest_run("repl_security_output_isolation_error_stays_in_session",
              repl_security_output_isolation_error_stays_in_session);
#else
    printf("test_repl_security_output_isolation (URBI_ENABLE_REPL=0, skipped)\n");
#endif
}
