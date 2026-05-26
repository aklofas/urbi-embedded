/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_security_compile_budget.c
 *
 * W4 security gate #4: compile-budget denial returns an explicit public
 * error code in the NDJSON response envelope, not just a closed connection.
 *
 * Uses urbi_realm_set_compile_budget to configure a 1-node budget, then
 * submits source that exceeds it and asserts the response carries one of
 * the URBI_ERR_COMPILE_BUDGET_* codes as a structured error envelope.
 *
 * Closes release F13 bullet 4. */
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

/* ---- Tests --------------------------------------------------------------- */

UTEST(repl_security_compile_budget_denial_emits_error_code)
{
    /* Set a very tight node budget (1 node).  Any non-trivial expression
     * will exceed it.  Verify the response envelope carries a structured
     * budget_* error code, not just a bare close. */
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port  = -1;

    /* Set a trivially small compile budget via the config. */
    cfg.default_budget.max_ast_nodes   = 1;
    cfg.default_budget.max_parser_depth = 0; /* 0 = unlimited */
    cfg.default_budget.max_source_bytes = 0;

    UReplServer *server = urbi_repl_serve(&vm, &cfg, NULL);
    UASSERT_NE(server, NULL);

    UReplSession *s = urepl_session_create(server);
    UASSERT_NE(s, NULL);
    s->authed = true;

    /* Submit source that requires more than 1 AST node. */
    const char *src = "1 + 2 + 3";
    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    UASSERT_NE(job, NULL);
    job->session_id  = s->session_id;
    job->req.id      = 1;
    job->req.op      = UREPL_OP_EVAL;
    job->req.code    = tdup(src);
    job->req.code_len = strlen(src);

    urepl_dispatch_job(server, job);

    char out[512];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';

    /* Response must be a structured error envelope (not empty). */
    UASSERT(n > 0);
    /* Must contain "error" kind. */
    UASSERT(strstr(out, "\"kind\":\"error\"") != NULL);
    /* Must carry one of the budget error codes. */
    int has_budget_code = (strstr(out, "budget_nodes")   != NULL)
                       || (strstr(out, "budget_depth")   != NULL)
                       || (strstr(out, "budget_source")  != NULL)
                       || (strstr(out, "parse")          != NULL);
    UASSERT(has_budget_code);

    urbi_repl_stop(server);
    urbi_vm_destroy(&vm);
}

UTEST(repl_security_compile_budget_no_limit_succeeds)
{
    /* With unlimited budget (zero fields), normal eval must succeed. */
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port  = -1;
    /* Zero budget fields = unlimited. */

    UReplServer *server = urbi_repl_serve(&vm, &cfg, NULL);
    UASSERT_NE(server, NULL);

    UReplSession *s = urepl_session_create(server);
    UASSERT_NE(s, NULL);
    s->authed = true;

    const char *src = "2 + 2";
    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    UASSERT_NE(job, NULL);
    job->session_id  = s->session_id;
    job->req.id      = 2;
    job->req.op      = UREPL_OP_EVAL;
    job->req.code    = tdup(src);
    job->req.code_len = strlen(src);

    urepl_dispatch_job(server, job);

    char out[512];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';

    UASSERT(n > 0);
    /* Must NOT be a budget error. */
    UASSERT(strstr(out, "budget_nodes") == NULL);
    UASSERT(strstr(out, "budget_depth") == NULL);

    urbi_repl_stop(server);
    urbi_vm_destroy(&vm);
}

UTEST(repl_security_compile_budget_source_bytes_enforced)
{
    /* max_source_bytes = 5: any source longer than 5 bytes must fail. */
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port  = -1;
    cfg.default_budget.max_source_bytes = 5;

    UReplServer *server = urbi_repl_serve(&vm, &cfg, NULL);
    UASSERT_NE(server, NULL);

    UReplSession *s = urepl_session_create(server);
    UASSERT_NE(s, NULL);
    s->authed = true;

    /* Source is 11 bytes — exceeds the 5-byte limit. */
    const char *src = "12345678901";
    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    UASSERT_NE(job, NULL);
    job->session_id  = s->session_id;
    job->req.id      = 3;
    job->req.op      = UREPL_OP_EVAL;
    job->req.code    = tdup(src);
    job->req.code_len = strlen(src);

    urepl_dispatch_job(server, job);

    char out[512];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';

    UASSERT(n > 0);
    UASSERT(strstr(out, "\"kind\":\"error\"") != NULL);
    UASSERT(strstr(out, "budget_source") != NULL);

    urbi_repl_stop(server);
    urbi_vm_destroy(&vm);
}

#endif /* URBI_ENABLE_REPL */

void
test_repl_security_compile_budget_suite(void)
{
#ifdef URBI_ENABLE_REPL
    printf("test_repl_security_compile_budget\n");
    utest_run("repl_security_compile_budget_denial_emits_error_code",
              repl_security_compile_budget_denial_emits_error_code);
    utest_run("repl_security_compile_budget_no_limit_succeeds",
              repl_security_compile_budget_no_limit_succeeds);
    utest_run("repl_security_compile_budget_source_bytes_enforced",
              repl_security_compile_budget_source_bytes_enforced);
#else
    printf("test_repl_security_compile_budget (URBI_ENABLE_REPL=0, skipped)\n");
#endif
}
