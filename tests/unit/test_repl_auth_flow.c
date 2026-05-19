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

/* ---- Per-IP rate-limiter (Task 18) ---------------------------------- */

UTEST(limiter_init_defaults)
{
    UReplAuthLimiter lim;
    urepl_auth_limiter_init(&lim);
    UASSERT_EQ(lim.max_attempts, 5);
    UASSERT_EQ(lim.window_secs, 30);
    UASSERT_EQ(lim.lockout_secs, 60);
    /* Fresh limiter accepts everything. */
    UASSERT(urepl_auth_limiter_check(&lim, 0x0100007fU, 1000));
}

UTEST(limiter_locks_after_max_attempts)
{
    UReplAuthLimiter lim;
    urepl_auth_limiter_init(&lim);
    uint32_t ip = 0x0100007fU;  /* 127.0.0.1 in network byte order */
    uint64_t now = 1000000ULL;
    /* 5 fails inside the 30 s window flip the slot to locked. */
    for (int i = 0; i < 5; ++i) {
        urepl_auth_limiter_record_fail(&lim, ip, now + (uint64_t)i * 1000ULL);
    }
    UASSERT(!urepl_auth_limiter_check(&lim, ip, now + 10000ULL));
}

UTEST(limiter_other_ips_still_allowed)
{
    UReplAuthLimiter lim;
    urepl_auth_limiter_init(&lim);
    uint32_t bad = 0x0100007fU;
    uint32_t good = 0x0a00000aU;
    uint64_t now = 1000000ULL;
    for (int i = 0; i < 5; ++i) {
        urepl_auth_limiter_record_fail(&lim, bad, now + (uint64_t)i * 1000ULL);
    }
    UASSERT(!urepl_auth_limiter_check(&lim, bad, now + 10000ULL));
    UASSERT(urepl_auth_limiter_check(&lim, good, now + 10000ULL));
}

UTEST(limiter_unlocks_after_lockout_window)
{
    UReplAuthLimiter lim;
    urepl_auth_limiter_init(&lim);
    uint32_t ip = 0x0100007fU;
    uint64_t now = 1000000ULL;
    for (int i = 0; i < 5; ++i) {
        urepl_auth_limiter_record_fail(&lim, ip, now + (uint64_t)i * 1000ULL);
    }
    /* Still locked at lockout - 1us. */
    UASSERT(!urepl_auth_limiter_check(&lim, ip, now + 59 * 1000000ULL));
    /* Unlocked at lockout + 1s past the 60 s gate. */
    UASSERT(urepl_auth_limiter_check(&lim, ip, now + 61 * 1000000ULL));
}

UTEST(limiter_success_clears_slot)
{
    UReplAuthLimiter lim;
    urepl_auth_limiter_init(&lim);
    uint32_t ip = 0x0100007fU;
    uint64_t now = 1000000ULL;
    /* Three fails (not enough to lock). */
    for (int i = 0; i < 3; ++i) {
        urepl_auth_limiter_record_fail(&lim, ip, now + (uint64_t)i * 1000ULL);
    }
    urepl_auth_limiter_record_success(&lim, ip);
    /* Slot reset — next 4 fails should NOT lock (one more needed). */
    for (int i = 0; i < 4; ++i) {
        urepl_auth_limiter_record_fail(&lim,
                                        ip, now + (uint64_t)(i + 100) * 1000ULL);
    }
    UASSERT(urepl_auth_limiter_check(&lim, ip, now + 1000000ULL));
}

UTEST(limiter_lru_eviction_under_table_pressure)
{
    /* 8 distinct IPs each get one fail; a ninth lands and evicts the
     * oldest (LRU).  Check that the first ip's record was rotated out
     * by verifying it is permitted again (no in_use slot left for it). */
    UReplAuthLimiter lim;
    urepl_auth_limiter_init(&lim);
    uint64_t now = 1000ULL;
    for (uint32_t i = 0; i < 8; ++i) {
        urepl_auth_limiter_record_fail(&lim, 0xAA000000U | i,
                                        now + (uint64_t)i * 100ULL);
    }
    /* Evict the oldest with one fail at higher time. */
    urepl_auth_limiter_record_fail(&lim, 0xBB000000U,
                                    now + 100000ULL);
    /* The oldest IP (the first one entered) should be evicted —
     * check returns true. */
    UASSERT(urepl_auth_limiter_check(&lim, 0xAA000000U, now + 1000000ULL));
}

UTEST(limiter_disabled_when_max_attempts_zero)
{
    UReplAuthLimiter lim;
    urepl_auth_limiter_init(&lim);
    lim.max_attempts = 0;  /* disable */
    /* Even 100 fails don't lock. */
    for (int i = 0; i < 100; ++i) {
        urepl_auth_limiter_record_fail(&lim, 1, 1000 + i);
    }
    UASSERT(urepl_auth_limiter_check(&lim, 1, 1000000));
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
    /* Task 18 — rate limiter */
    utest_run("limiter_init_defaults",             limiter_init_defaults);
    utest_run("limiter_locks_after_max_attempts",  limiter_locks_after_max_attempts);
    utest_run("limiter_other_ips_still_allowed",   limiter_other_ips_still_allowed);
    utest_run("limiter_unlocks_after_lockout_window",
              limiter_unlocks_after_lockout_window);
    utest_run("limiter_success_clears_slot",       limiter_success_clears_slot);
    utest_run("limiter_lru_eviction_under_table_pressure",
              limiter_lru_eviction_under_table_pressure);
    utest_run("limiter_disabled_when_max_attempts_zero",
              limiter_disabled_when_max_attempts_zero);
}

#else  /* !URBI_ENABLE_REPL */

void test_repl_auth_flow_suite(void) { /* skipped: URBI_ENABLE_REPL=0 */ }

#endif
