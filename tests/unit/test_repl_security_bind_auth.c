/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_security_bind_auth.c
 *
 * W4 security gates #1-2:
 *   test_repl_security_loopback_no_token  — non-loopback bind without token
 *                                           MUST fail at urbi_repl_serve.
 *   test_repl_security_token_accept_reject — token accepted on match; rejected
 *                                            with explicit error envelope on
 *                                            mismatch + session torn down.
 *
 * Closes release F13 bullets 1-2. */
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

/* strdup is POSIX (not C99). */
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

/* Build a loopback server with the given token (may be NULL). */
static UReplServer *
mk_server(UVM *vm, const char *bind_addr, const char *token)
{
    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = bind_addr;
    cfg.tcp_port  = -1;   /* no real socket */
    cfg.auth_token = token;
    int err = 0;
    UReplServer *s = urbi_repl_serve(vm, &cfg, &err);
    (void)err;
    return s;
}

/* ---- Test #1: non-loopback bind without token MUST fail -------------- */

UTEST(repl_security_loopback_no_token_fails_with_insecure_config)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr  = "0.0.0.0";  /* non-loopback */
    cfg.tcp_port   = -1;
    cfg.auth_token = NULL;        /* no token */

    int err = 0;
    UReplServer *srv = urbi_repl_serve(&vm, &cfg, &err);

    UASSERT_EQ(srv, NULL);
    /* Must return URBI_ERR_INSECURE_CONFIG (== URBI_ERR_INVALID_CONFIG). */
    UASSERT_EQ(err, URBI_ERR_INSECURE_CONFIG);

    urbi_vm_destroy(&vm);
}

UTEST(repl_security_nonloopback_with_token_succeeds)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr  = "0.0.0.0";
    cfg.tcp_port   = -1;
    cfg.auth_token = "sekrit";

    int err = 0;
    UReplServer *srv = urbi_repl_serve(&vm, &cfg, &err);

    UASSERT_EQ(err, URBI_OK);
    UASSERT_NE(srv, NULL);

    urbi_repl_stop(srv);
    urbi_vm_destroy(&vm);
}

UTEST(repl_security_loopback_no_token_ok)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    int err = 0;
    UReplServer *srv = mk_server(&vm, "127.0.0.1", NULL);
    UASSERT_NE(srv, NULL);
    (void)err;

    urbi_repl_stop(srv);
    urbi_vm_destroy(&vm);
}

UTEST(repl_security_null_bind_addr_no_token_ok)
{
    /* NULL bind_addr defaults to loopback — must be allowed without token. */
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr  = NULL;
    cfg.tcp_port   = -1;
    cfg.auth_token = NULL;

    int err = 0;
    UReplServer *srv = urbi_repl_serve(&vm, &cfg, &err);
    UASSERT_EQ(err, URBI_OK);
    UASSERT_NE(srv, NULL);

    urbi_repl_stop(srv);
    urbi_vm_destroy(&vm);
}

UTEST(repl_security_ipv6_loopback_no_token_ok)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    int err = 0;
    UReplServer *srv = mk_server(&vm, "::1", NULL);
    UASSERT_NE(srv, NULL);
    (void)err;

    urbi_repl_stop(srv);
    urbi_vm_destroy(&vm);
}

/* ---- Test #2: token accept/reject at dispatch level ------------------ */

UTEST(repl_security_token_match_accepts)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    UReplServer *server = mk_server(&vm, "127.0.0.1", "abc123");
    UASSERT_NE(server, NULL);

    UReplSession *s = urepl_session_create(server);
    UASSERT_NE(s, NULL);
    UASSERT_EQ(s->authed, false);

    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    UASSERT_NE(job, NULL);
    job->session_id = s->session_id;
    job->req.id     = 1;
    job->req.op     = UREPL_OP_AUTH;
    job->req.token  = tdup("abc123");

    urepl_dispatch_job(server, job);

    UASSERT_EQ(s->authed, true);

    char out[256];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(strstr(out, "\"kind\":\"auth_ok\"") != NULL);

    urbi_repl_stop(server);
    urbi_vm_destroy(&vm);
}

UTEST(repl_security_token_mismatch_rejects_emits_error_and_closes)
{
    /* On token mismatch: explicit auth_failed envelope emitted AND
     * session is scheduled for teardown (needs_teardown set). */
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    UReplServer *server = mk_server(&vm, "127.0.0.1", "right_token");
    UASSERT_NE(server, NULL);

    UReplSession *s = urepl_session_create(server);
    UASSERT_NE(s, NULL);

    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    UASSERT_NE(job, NULL);
    job->session_id = s->session_id;
    job->req.id     = 2;
    job->req.op     = UREPL_OP_AUTH;
    job->req.token  = tdup("wrong_token");

    urepl_dispatch_job(server, job);

    /* Session must NOT be authed. */
    UASSERT_EQ(s->authed, false);

    /* Output must contain explicit error code. */
    char out[256];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(strstr(out, "\"code\":\"auth_failed\"") != NULL);

    /* Connection must be scheduled for teardown — not silently hanging. */
    UASSERT_EQ(s->needs_teardown, true);

    urbi_repl_stop(server);
    urbi_vm_destroy(&vm);
}

UTEST(repl_security_null_token_rejects_and_closes)
{
    /* Omitted token field (NULL) also triggers auth_failed + teardown. */
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    UReplServer *server = mk_server(&vm, "127.0.0.1", "tok");
    UASSERT_NE(server, NULL);

    UReplSession *s = urepl_session_create(server);
    UASSERT_NE(s, NULL);

    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    UASSERT_NE(job, NULL);
    job->session_id = s->session_id;
    job->req.id     = 3;
    job->req.op     = UREPL_OP_AUTH;
    /* job->req.token == NULL */

    urepl_dispatch_job(server, job);

    UASSERT_EQ(s->authed, false);
    char out[256];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(strstr(out, "\"code\":\"auth_failed\"") != NULL);
    UASSERT_EQ(s->needs_teardown, true);

    urbi_repl_stop(server);
    urbi_vm_destroy(&vm);
}

#endif /* URBI_ENABLE_REPL */

void
test_repl_security_bind_auth_suite(void)
{
#ifdef URBI_ENABLE_REPL
    printf("test_repl_security_bind_auth\n");
    utest_run("repl_security_loopback_no_token_fails_with_insecure_config",
              repl_security_loopback_no_token_fails_with_insecure_config);
    utest_run("repl_security_nonloopback_with_token_succeeds",
              repl_security_nonloopback_with_token_succeeds);
    utest_run("repl_security_loopback_no_token_ok",
              repl_security_loopback_no_token_ok);
    utest_run("repl_security_null_bind_addr_no_token_ok",
              repl_security_null_bind_addr_no_token_ok);
    utest_run("repl_security_ipv6_loopback_no_token_ok",
              repl_security_ipv6_loopback_no_token_ok);
    utest_run("repl_security_token_match_accepts",
              repl_security_token_match_accepts);
    utest_run("repl_security_token_mismatch_rejects_emits_error_and_closes",
              repl_security_token_mismatch_rejects_emits_error_and_closes);
    utest_run("repl_security_null_token_rejects_and_closes",
              repl_security_null_token_rejects_and_closes);
#else
    printf("test_repl_security_bind_auth (URBI_ENABLE_REPL=0, skipped)\n");
#endif
}
