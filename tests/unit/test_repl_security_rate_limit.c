/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_security_rate_limit.c
 *
 * W4 security gate #3: per-source job rate limit.
 *
 * rate_limit_per_second in UReplConfig caps the number of NDJSON jobs
 * accepted per session per clock-second.  Excess jobs receive an explicit
 * rate_limit_exceeded error and the session is torn down.
 *
 * Closes release F13 bullet 3. */
#include "utest.h"

#ifdef URBI_ENABLE_REPL
#ifndef URBI_REPL_COOPERATIVE_ONLY  /* rate limit is POSIX-only */

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

/* Build a server with rate_limit_per_second. */
static UReplServer *
mk_rate_server(UVM *vm, int rate_limit)
{
    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr          = "127.0.0.1";
    cfg.tcp_port           = -1;
    cfg.auth_token         = NULL;
    cfg.rate_limit_per_second = rate_limit;
    return urbi_repl_serve(vm, &cfg, NULL);
}

/* Push N loopback eval jobs to a session; drain output; return how many
 * got rate-limited (saw "rate_limit_exceeded" in output). */
static int
count_rate_limited(UReplServer *server, UReplSession *s, int n)
{
    int limited = 0;
    for (int i = 0; i < n; i++) {
        UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
        if (job == NULL) { break; }
        job->session_id = s->session_id;
        job->req.id     = (uint64_t)(i + 1);
        job->req.op     = UREPL_OP_EVAL;
        job->req.code   = tdup("1");
        job->req.code_len = 1;
        s->authed = true;  /* bypass auth gate for this test */
        urepl_dispatch_job(server, job);

        char out[512];
        size_t n2 = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
        out[n2] = '\0';
        if (strstr(out, "rate_limit_exceeded") != NULL) {
            limited++;
        }
    }
    return limited;
}

/* ---- Tests --------------------------------------------------------------- */

UTEST(repl_security_rate_limit_zero_means_unlimited)
{
    /* rate_limit_per_second == 0 → unlimited. */
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    UReplServer *server = mk_rate_server(&vm, 0);
    UASSERT_NE(server, NULL);

    UReplSession *s = urepl_session_create(server);
    UASSERT_NE(s, NULL);

    int limited = count_rate_limited(server, s, 20);
    /* With no limit, none should be rate-limited. */
    UASSERT_EQ(limited, 0);

    urbi_repl_stop(server);
    urbi_vm_destroy(&vm);
}

UTEST(repl_security_rate_limit_enforced_under_burst)
{
    /* rate_limit_per_second == 5: first 5 jobs in the same clock-second
     * are accepted; the 6th triggers a rate_limit_exceeded error and
     * session teardown.  All 10 dispatches happen within one second on
     * any hardware that can run the test at all. */
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    UReplServer *server = mk_rate_server(&vm, 5);
    UASSERT_NE(server, NULL);

    UReplSession *s = urepl_session_create(server);
    UASSERT_NE(s, NULL);

    int limited = count_rate_limited(server, s, 10);
    /* At least 1 job should have been rate-limited (the 6th). */
    UASSERT(limited >= 1);

    urbi_repl_stop(server);
    urbi_vm_destroy(&vm);
}

UTEST(repl_security_rate_limit_session_torn_down_after_limit)
{
    /* After the rate limit triggers, the session must have needs_teardown
     * set (clean close, not a hang). */
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    UReplServer *server = mk_rate_server(&vm, 2);
    UASSERT_NE(server, NULL);

    UReplSession *s = urepl_session_create(server);
    UASSERT_NE(s, NULL);

    /* Send 5 jobs — the 3rd must exceed the limit. */
    (void)count_rate_limited(server, s, 5);

    /* Session must be scheduled for teardown. */
    UASSERT_EQ(s->needs_teardown, true);

    urbi_repl_stop(server);
    urbi_vm_destroy(&vm);
}

UTEST(repl_security_rate_limit_explicit_error_envelope)
{
    /* The rate_limit_exceeded response must be a valid error envelope,
     * not a bare close. */
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);
    UReplServer *server = mk_rate_server(&vm, 1);
    UASSERT_NE(server, NULL);

    UReplSession *s = urepl_session_create(server);
    UASSERT_NE(s, NULL);

    /* Burn the one-per-second budget. */
    s->authed = true;
    UReplJob *j1 = (UReplJob *)calloc(1, sizeof(*j1));
    j1->session_id = s->session_id;
    j1->req.id     = 1;
    j1->req.op     = UREPL_OP_EVAL;
    j1->req.code   = tdup("1");
    j1->req.code_len = 1;
    urepl_dispatch_job(server, j1);
    char discard[256];
    (void)urepl_ringbuf_read(&s->output, discard, sizeof(discard));

    /* Second job in the same second — must get rate error. */
    UReplJob *j2 = (UReplJob *)calloc(1, sizeof(*j2));
    j2->session_id = s->session_id;
    j2->req.id     = 2;
    j2->req.op     = UREPL_OP_EVAL;
    j2->req.code   = tdup("2");
    j2->req.code_len = 1;
    urepl_dispatch_job(server, j2);

    char out[256];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    /* Must contain structured error fields. */
    UASSERT(strstr(out, "\"kind\":\"error\"") != NULL);
    UASSERT(strstr(out, "rate_limit_exceeded") != NULL);

    urbi_repl_stop(server);
    urbi_vm_destroy(&vm);
}

#endif /* !URBI_REPL_COOPERATIVE_ONLY */
#endif /* URBI_ENABLE_REPL */

void
test_repl_security_rate_limit_suite(void)
{
#ifdef URBI_ENABLE_REPL
#ifndef URBI_REPL_COOPERATIVE_ONLY
    printf("test_repl_security_rate_limit\n");
    utest_run("repl_security_rate_limit_zero_means_unlimited",
              repl_security_rate_limit_zero_means_unlimited);
    utest_run("repl_security_rate_limit_enforced_under_burst",
              repl_security_rate_limit_enforced_under_burst);
    utest_run("repl_security_rate_limit_session_torn_down_after_limit",
              repl_security_rate_limit_session_torn_down_after_limit);
    utest_run("repl_security_rate_limit_explicit_error_envelope",
              repl_security_rate_limit_explicit_error_envelope);
#else
    printf("test_repl_security_rate_limit (COOPERATIVE_ONLY, skipped)\n");
#endif
#else
    printf("test_repl_security_rate_limit (URBI_ENABLE_REPL=0, skipped)\n");
#endif
}
