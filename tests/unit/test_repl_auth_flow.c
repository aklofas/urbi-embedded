/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_auth_flow.c — bearer-token compare + auth gate.
 *
 * Task 17 lands the constant-time compare + dispatch path; Task 18
 * extends with per-IP rate-limit tests in the same suite. */
#include "utest.h"

#ifdef URBI_ENABLE_REPL

#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "repl/urepl_auth.h"
#include "repl/urepl_dispatch.h"
#include "repl/urepl_queue.h"
#include "repl/urepl.h"

#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ---- Constant-time compare ------------------------------------------ */

UTEST(auth_token_match_equal)
{
    UASSERT(urepl_auth_token_match("hunter2", 7, "hunter2", 7));
}

UTEST(auth_token_match_one_byte_diff)
{
    UASSERT(!urepl_auth_token_match("hunter2", 7, "hunter3", 7));
}

UTEST(auth_token_match_length_diff)
{
    UASSERT(!urepl_auth_token_match("hunter2", 7, "hunter22", 8));
    UASSERT(!urepl_auth_token_match("", 0, "x", 1));
}

UTEST(auth_token_match_null_args)
{
    UASSERT(!urepl_auth_token_match(NULL, 0, "x", 1));
    UASSERT(!urepl_auth_token_match("x", 1, NULL, 0));
}

UTEST(auth_token_match_both_empty)
{
    UASSERT(urepl_auth_token_match("", 0, "", 0));
}

UTEST(auth_token_match_binary_safe)
{
    /* Embedded NUL must not short-circuit (the constant-time impl is
     * fully length-driven, not strcmp-shaped). */
    const char a[] = { 'a', '\0', 'b' };
    const char b[] = { 'a', '\0', 'b' };
    const char c[] = { 'a', '\0', 'c' };
    UASSERT(urepl_auth_token_match(a, 3, b, 3));
    UASSERT(!urepl_auth_token_match(a, 3, c, 3));
}

/* ---- Dispatcher auth flow ------------------------------------------- */

/* Helper: build a server with a fixed token. */
static UReplServer *
mk_token_server(UVM **out_vm, const char *token)
{
    UVM *vm = (UVM *)calloc(1, sizeof(UVM));
    urbi_vm_init(vm, NULL, NULL);
    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port = -1;
    cfg.auth_token = token;
    UReplServer *server = urbi_repl_serve(vm, &cfg, NULL);
    *out_vm = vm;
    return server;
}

static void
free_server(UReplServer *server, UVM *vm)
{
    urbi_repl_stop(server);
    urbi_vm_destroy(vm);
    free(vm);
}

UTEST(auth_op_with_correct_token_grants_authed)
{
    UVM *vm = NULL;
    UReplServer *server = mk_token_server(&vm, "sekret");
    UReplSession *s = urepl_session_create(server);
    UASSERT(s->authed == false);

    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    job->session_id = s->session_id;
    job->req.id = 1;
    job->req.op = UREPL_OP_AUTH;
    job->req.token = strdup("sekret");
    urepl_dispatch_job(server, job);
    UASSERT(s->authed == true);

    char out[256];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(strstr(out, "\"kind\":\"auth_ok\"") != NULL);
    free_server(server, vm);
}

UTEST(auth_op_with_wrong_token_rejects)
{
    UVM *vm = NULL;
    UReplServer *server = mk_token_server(&vm, "sekret");
    UReplSession *s = urepl_session_create(server);

    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    job->session_id = s->session_id;
    job->req.id = 2;
    job->req.op = UREPL_OP_AUTH;
    job->req.token = strdup("wrong");
    urepl_dispatch_job(server, job);
    UASSERT(s->authed == false);

    char out[256];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(strstr(out, "\"code\":\"auth_failed\"") != NULL);
    free_server(server, vm);
}

UTEST(auth_op_with_null_token_rejects)
{
    /* Token field omitted from the request — must reject (not crash). */
    UVM *vm = NULL;
    UReplServer *server = mk_token_server(&vm, "sekret");
    UReplSession *s = urepl_session_create(server);

    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    job->session_id = s->session_id;
    job->req.id = 3;
    job->req.op = UREPL_OP_AUTH;
    /* job->req.token = NULL */
    urepl_dispatch_job(server, job);
    UASSERT(s->authed == false);

    char out[256];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(strstr(out, "\"code\":\"auth_failed\"") != NULL);
    free_server(server, vm);
}

UTEST(preauth_eval_rejected_when_token_required)
{
    /* Pre-auth eval must emit auth_required, not run the eval. */
    UVM *vm = NULL;
    UReplServer *server = mk_token_server(&vm, "tok");
    UReplSession *s = urepl_session_create(server);
    UASSERT(s->authed == false);

    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    job->session_id = s->session_id;
    job->req.id = 4;
    job->req.op = UREPL_OP_EVAL;
    job->req.code = strdup("1+1");
    job->req.code_len = 3;
    urepl_dispatch_job(server, job);

    char out[256];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(strstr(out, "\"code\":\"auth_required\"") != NULL);
    /* No result envelope — the eval was NOT run. */
    UASSERT(strstr(out, "\"kind\":\"result\"") == NULL);
    free_server(server, vm);
}

UTEST(post_auth_eval_runs)
{
    /* Full happy path: auth then eval. */
    UVM *vm = NULL;
    UReplServer *server = mk_token_server(&vm, "tok");
    UReplSession *s = urepl_session_create(server);

    /* Step 1: auth */
    UReplJob *a = (UReplJob *)calloc(1, sizeof(*a));
    a->session_id = s->session_id;
    a->req.id = 5;
    a->req.op = UREPL_OP_AUTH;
    a->req.token = strdup("tok");
    urepl_dispatch_job(server, a);
    UASSERT(s->authed == true);

    /* Step 2: eval (drain auth_ok first to keep the buf clean). */
    char auth_out[64];
    (void)urepl_ringbuf_read(&s->output, auth_out, sizeof(auth_out));

    UReplJob *e = (UReplJob *)calloc(1, sizeof(*e));
    e->session_id = s->session_id;
    e->req.id = 6;
    e->req.op = UREPL_OP_EVAL;
    e->req.code = strdup("2+3");
    e->req.code_len = 3;
    urepl_dispatch_job(server, e);

    char out[512];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(strstr(out, "\"kind\":\"result\"") != NULL);
    UASSERT(strstr(out, "\"value\":\"5\"") != NULL);
    free_server(server, vm);
}

void
test_repl_auth_flow_suite(void)
{
    printf("test_repl_auth_flow\n");
    utest_run("auth_token_match_equal",            auth_token_match_equal);
    utest_run("auth_token_match_one_byte_diff",    auth_token_match_one_byte_diff);
    utest_run("auth_token_match_length_diff",      auth_token_match_length_diff);
    utest_run("auth_token_match_null_args",        auth_token_match_null_args);
    utest_run("auth_token_match_both_empty",       auth_token_match_both_empty);
    utest_run("auth_token_match_binary_safe",      auth_token_match_binary_safe);
    utest_run("auth_op_with_correct_token_grants_authed",
              auth_op_with_correct_token_grants_authed);
    utest_run("auth_op_with_wrong_token_rejects",  auth_op_with_wrong_token_rejects);
    utest_run("auth_op_with_null_token_rejects",   auth_op_with_null_token_rejects);
    utest_run("preauth_eval_rejected_when_token_required",
              preauth_eval_rejected_when_token_required);
    utest_run("post_auth_eval_runs",               post_auth_eval_runs);
}

#else  /* !URBI_ENABLE_REPL */

void test_repl_auth_flow_suite(void) { /* skipped: URBI_ENABLE_REPL=0 */ }

#endif
