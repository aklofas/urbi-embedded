/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_dispatcher.c — REPL queue + ringbuf + dispatcher.
 *
 * Task 12 lands the queue + ringbuf cases; Task 13 extends with full
 * dispatcher round-trip tests. */
#include "utest.h"

#ifdef URBI_ENABLE_REPL

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "repl/urepl_queue.h"
#include "repl/urepl_dispatch.h"
#include "repl/urepl.h"
#include "repl/urepl_listener.h"
#include "repl/urepl_buffer_transport.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* strdup is POSIX (not C99); avoid implicit declaration truncation under
 * -std=c99 -Wpedantic.  This helper matches test_vm.c's pattern. */
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

/* ---- Queue tests ----------------------------------------------------- */

UTEST(queue_init_destroy_empty)
{
    UReplQueue q;
    UASSERT_EQ(urepl_queue_init(&q), URBI_OK);
    UASSERT_EQ(urepl_queue_count(&q), 0);
    urepl_queue_destroy(&q);
}

UTEST(queue_push_drain_roundtrip)
{
    UReplQueue q;
    UASSERT_EQ(urepl_queue_init(&q), URBI_OK);

    UReplJob *j1 = (UReplJob *)calloc(1, sizeof(*j1));
    UReplJob *j2 = (UReplJob *)calloc(1, sizeof(*j2));
    UReplJob *j3 = (UReplJob *)calloc(1, sizeof(*j3));
    j1->session_id = 1;
    j2->session_id = 2;
    j3->session_id = 3;
    UASSERT_EQ(urepl_queue_push(&q, j1), URBI_OK);
    UASSERT_EQ(urepl_queue_push(&q, j2), URBI_OK);
    UASSERT_EQ(urepl_queue_push(&q, j3), URBI_OK);
    UASSERT_EQ(urepl_queue_count(&q), 3);

    UReplJob *drained = urepl_queue_drain_all(&q);
    UASSERT(drained != NULL);
    UASSERT_EQ(drained->session_id, 1);
    UASSERT(drained->next != NULL);
    UASSERT_EQ(drained->next->session_id, 2);
    UASSERT(drained->next->next != NULL);
    UASSERT_EQ(drained->next->next->session_id, 3);
    UASSERT(drained->next->next->next == NULL);
    UASSERT_EQ(urepl_queue_count(&q), 0);

    while (drained != NULL) {
        UReplJob *next = drained->next;
        urepl_ndjson_free_req(&drained->req);
        free(drained);
        drained = next;
    }
    urepl_queue_destroy(&q);
}

UTEST(queue_drain_empty_returns_null)
{
    UReplQueue q;
    UASSERT_EQ(urepl_queue_init(&q), URBI_OK);
    UASSERT(urepl_queue_drain_all(&q) == NULL);
    urepl_queue_destroy(&q);
}

UTEST(queue_destroy_frees_remaining_jobs)
{
    /* Ensures destroy doesn't leak the leftover jobs.  Verified via
     * absence of AddressSanitizer/valgrind complaints in CI; the
     * functional smoke is just that destroy returns. */
    UReplQueue q;
    UASSERT_EQ(urepl_queue_init(&q), URBI_OK);
    for (int i = 0; i < 5; i++) {
        UReplJob *j = (UReplJob *)calloc(1, sizeof(*j));
        j->session_id = (uint32_t)(i + 1);
        UASSERT_EQ(urepl_queue_push(&q, j), URBI_OK);
    }
    UASSERT_EQ(urepl_queue_count(&q), 5);
    urepl_queue_destroy(&q);  /* must free the 5 jobs internally */
}

/* MPSC stress: 4 producer threads each push 100 jobs; consumer drains
 * twice and counts total.  Verifies the count equals 400 and no jobs
 * are lost or duplicated. */
typedef struct {
    UReplQueue *q;
    int         producer_id;
    int         n;
} ProdArgs;

static void *
producer_thread(void *arg)
{
    ProdArgs *a = (ProdArgs *)arg;
    for (int i = 0; i < a->n; i++) {
        UReplJob *j = (UReplJob *)calloc(1, sizeof(*j));
        if (j == NULL) {
            return NULL;
        }
        /* session_id encodes (producer << 16 | seq) for uniqueness. */
        j->session_id = (uint32_t)((a->producer_id << 16) | i);
        urepl_queue_push(a->q, j);
    }
    return NULL;
}

UTEST(queue_mpsc_stress_4_producers_100_each)
{
    UReplQueue q;
    UASSERT_EQ(urepl_queue_init(&q), URBI_OK);

    pthread_t threads[4];
    ProdArgs args[4];
    for (int i = 0; i < 4; i++) {
        args[i].q = &q;
        args[i].producer_id = i;
        args[i].n = 100;
        pthread_create(&threads[i], NULL, producer_thread, &args[i]);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Consumer drains. */
    int seen = 0;
    UReplJob *drained = urepl_queue_drain_all(&q);
    while (drained != NULL) {
        UReplJob *next = drained->next;
        urepl_ndjson_free_req(&drained->req);
        free(drained);
        seen++;
        drained = next;
    }
    UASSERT_EQ(seen, 400);
    urepl_queue_destroy(&q);
}

/* ---- Ringbuf tests --------------------------------------------------- */

UTEST(ringbuf_init_destroy_empty)
{
    UReplRingbuf rb;
    UASSERT_EQ(urepl_ringbuf_init(&rb, 64), URBI_OK);
    UASSERT_EQ(urepl_ringbuf_fill(&rb), 0);
    UASSERT_EQ(urepl_ringbuf_overflow(&rb), 0);
    urepl_ringbuf_destroy(&rb);
}

UTEST(ringbuf_write_read_basic)
{
    UReplRingbuf rb;
    UASSERT_EQ(urepl_ringbuf_init(&rb, 64), URBI_OK);
    size_t n = urepl_ringbuf_write(&rb, "hello", 5);
    UASSERT_EQ(n, 5);
    UASSERT_EQ(urepl_ringbuf_fill(&rb), 5);
    char out[32];
    n = urepl_ringbuf_read(&rb, out, sizeof(out));
    UASSERT_EQ(n, 5);
    UASSERT(memcmp(out, "hello", 5) == 0);
    UASSERT_EQ(urepl_ringbuf_fill(&rb), 0);
    urepl_ringbuf_destroy(&rb);
}

UTEST(ringbuf_partial_read)
{
    UReplRingbuf rb;
    UASSERT_EQ(urepl_ringbuf_init(&rb, 64), URBI_OK);
    urepl_ringbuf_write(&rb, "abcdefghij", 10);
    char out[4];
    size_t n = urepl_ringbuf_read(&rb, out, 4);
    UASSERT_EQ(n, 4);
    UASSERT(memcmp(out, "abcd", 4) == 0);
    UASSERT_EQ(urepl_ringbuf_fill(&rb), 6);
    n = urepl_ringbuf_read(&rb, out, 4);
    UASSERT_EQ(n, 4);
    UASSERT(memcmp(out, "efgh", 4) == 0);
    urepl_ringbuf_destroy(&rb);
}

UTEST(ringbuf_wraps_around)
{
    /* Fill exactly, drain partial, write again forcing wraparound. */
    UReplRingbuf rb;
    UASSERT_EQ(urepl_ringbuf_init(&rb, 8), URBI_OK);
    urepl_ringbuf_write(&rb, "12345678", 8);
    UASSERT_EQ(urepl_ringbuf_fill(&rb), 8);
    char out[16];
    size_t n = urepl_ringbuf_read(&rb, out, 5);
    UASSERT_EQ(n, 5);
    UASSERT(memcmp(out, "12345", 5) == 0);
    UASSERT_EQ(urepl_ringbuf_fill(&rb), 3);
    /* Now write "ABCDE" — this wraps around. */
    urepl_ringbuf_write(&rb, "ABCDE", 5);
    UASSERT_EQ(urepl_ringbuf_fill(&rb), 8);
    n = urepl_ringbuf_read(&rb, out, 16);
    UASSERT_EQ(n, 8);
    UASSERT(memcmp(out, "678ABCDE", 8) == 0);
    urepl_ringbuf_destroy(&rb);
}

UTEST(ringbuf_overflow_drops_oldest)
{
    UReplRingbuf rb;
    UASSERT_EQ(urepl_ringbuf_init(&rb, 8), URBI_OK);
    urepl_ringbuf_write(&rb, "AAAAAAAA", 8);
    /* Overflow by 4 bytes.  REPL-02 frame-boundary fix: after the raw drop of 4
     * bytes, the scan continues consuming bytes until it finds '\n' or exhausts
     * the ring.  "AAAAAAAA" has no '\n', so the scan empties the ring; then "BBBB"
     * is written fresh.  Result: fill=4, content="BBBB". */
    urepl_ringbuf_write(&rb, "BBBB", 4);
    UASSERT(urepl_ringbuf_overflow(&rb) == true);
    UASSERT_EQ(urepl_ringbuf_fill(&rb), (size_t)4U);
    char out[16];
    size_t n = urepl_ringbuf_read(&rb, out, 16);
    UASSERT_EQ(n, (size_t)4U);
    UASSERT(memcmp(out, "BBBB", 4) == 0);
    urepl_ringbuf_destroy(&rb);
}

UTEST(ringbuf_write_larger_than_cap)
{
    UReplRingbuf rb;
    UASSERT_EQ(urepl_ringbuf_init(&rb, 4), URBI_OK);
    /* 6-byte write into 4-byte ringbuf retains the last 4 bytes. */
    urepl_ringbuf_write(&rb, "ABCDEF", 6);
    UASSERT(urepl_ringbuf_overflow(&rb) == true);
    UASSERT_EQ(urepl_ringbuf_fill(&rb), 4);
    char out[8];
    size_t n = urepl_ringbuf_read(&rb, out, 8);
    UASSERT_EQ(n, 4);
    UASSERT(memcmp(out, "CDEF", 4) == 0);
    urepl_ringbuf_destroy(&rb);
}

UTEST(ringbuf_read_empty_returns_zero)
{
    UReplRingbuf rb;
    UASSERT_EQ(urepl_ringbuf_init(&rb, 8), URBI_OK);
    char out[4];
    UASSERT_EQ(urepl_ringbuf_read(&rb, out, 4), 0);
    urepl_ringbuf_destroy(&rb);
}

UTEST(ringbuf_zero_size_init_rejects)
{
    UReplRingbuf rb;
    int rc = urepl_ringbuf_init(&rb, 0);
    UASSERT(rc != URBI_OK);
}

/* ---- Dispatcher tests (Task 13) -------------------------------------- */

/* Helper: create a loopback (no-auth) server backed by a fresh VM. */
static UReplServer *
mk_server(UVM **out_vm)
{
    UVM *vm = (UVM *)calloc(1, sizeof(UVM));
    if (vm == NULL) return NULL;
    if (urbi_vm_init(vm, NULL, NULL) != URBI_OK) {
        free(vm);
        return NULL;
    }
    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port = -1;
    int err = 0;
    UReplServer *server = urbi_repl_serve(vm, &cfg, &err);
    if (server == NULL) {
        urbi_vm_destroy(vm);
        free(vm);
        return NULL;
    }
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

UTEST(dispatcher_session_create_destroy)
{
    UVM *vm = NULL;
    UReplServer *server = mk_server(&vm);
    UASSERT(server != NULL);
    UReplSession *s = urepl_session_create(server);
    UASSERT(s != NULL);
    UASSERT(s->realm != NULL);
    UASSERT(s->session_id == 1);
    UASSERT(strlen(s->lobby_id_hex) == 4);
    /* Auto-applied REPL compile budget should be visible on the realm. */
    UASSERT(urbi_realm_get_compile_budget(vm, s->realm) != NULL);
    urepl_session_destroy(server, s);
    free_server(server, vm);
}

UTEST(dispatcher_session_find_by_id)
{
    UVM *vm = NULL;
    UReplServer *server = mk_server(&vm);
    UReplSession *s1 = urepl_session_create(server);
    UReplSession *s2 = urepl_session_create(server);
    UASSERT(s1 != s2);
    UASSERT(urepl_session_find(server, s1->session_id) == s1);
    UASSERT(urepl_session_find(server, s2->session_id) == s2);
    UASSERT(urepl_session_find(server, 9999) == NULL);
    UASSERT(urepl_session_find_by_lobby(server, s1->lobby_id_hex) == s1);
    UASSERT(urepl_session_find_by_lobby(server, "ffff") == NULL);
    free_server(server, vm);
}

UTEST(dispatcher_handles_eval_op)
{
    UVM *vm = NULL;
    UReplServer *server = mk_server(&vm);
    UReplSession *s = urepl_session_create(server);
    UASSERT(s != NULL);
    s->authed = true;

    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    job->session_id = s->session_id;
    job->req.id = 1;
    job->req.op = UREPL_OP_EVAL;
    job->req.code = (char *)malloc(6);
    memcpy(job->req.code, "1 + 2", 6);
    job->req.code_len = 5;

    urepl_dispatch_job(server, job);

    /* Drain output ringbuf — expect a result envelope then done. */
    char out[1024];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(n > 0);
    UASSERT(strstr(out, "\"kind\":\"result\"") != NULL);
    UASSERT(strstr(out, "\"value\":\"3\"") != NULL);
    UASSERT(strstr(out, "\"kind\":\"done\"") != NULL);
    UASSERT(strstr(out, "\"id\":1") != NULL);
    free_server(server, vm);
}

UTEST(dispatcher_eval_compile_error_emits_error_envelope)
{
    UVM *vm = NULL;
    UReplServer *server = mk_server(&vm);
    UReplSession *s = urepl_session_create(server);
    s->authed = true;

    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    job->session_id = s->session_id;
    job->req.id = 2;
    job->req.op = UREPL_OP_EVAL;
    /* Pathological input — open paren only. */
    job->req.code = (char *)malloc(4);
    memcpy(job->req.code, "1+(", 4);
    job->req.code_len = 3;
    urepl_dispatch_job(server, job);

    char out[1024];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(strstr(out, "\"kind\":\"error\"") != NULL);
    UASSERT(strstr(out, "\"kind\":\"done\"") != NULL);
    UASSERT(strstr(out, "\"id\":2") != NULL);
    free_server(server, vm);
}

UTEST(dispatcher_auth_op_without_token_grants)
{
    /* mk_server() leaves cfg.auth_token == NULL — auth becomes a no-op
     * success path (loopback default). */
    UVM *vm = NULL;
    UReplServer *server = mk_server(&vm);
    UReplSession *s = urepl_session_create(server);
    UASSERT(s->authed == false);

    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    job->session_id = s->session_id;
    job->req.id = 3;
    job->req.op = UREPL_OP_AUTH;
    /* No token; with auth_token unset this is still OK. */
    urepl_dispatch_job(server, job);
    UASSERT(s->authed == true);

    char out[256];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(strstr(out, "\"kind\":\"auth_ok\"") != NULL);
    free_server(server, vm);
}

UTEST(dispatcher_auth_op_with_correct_token)
{
    UVM *vm = (UVM *)calloc(1, sizeof(UVM));
    urbi_vm_init(vm, NULL, NULL);
    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port = -1;
    cfg.auth_token = "hunter2";
    int err = 0;
    UReplServer *server = urbi_repl_serve(vm, &cfg, &err);
    UASSERT(server != NULL);

    UReplSession *s = urepl_session_create(server);
    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    job->session_id = s->session_id;
    job->req.id = 4;
    job->req.op = UREPL_OP_AUTH;
    job->req.token = tdup("hunter2");
    urepl_dispatch_job(server, job);
    UASSERT(s->authed == true);

    char out[256];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(strstr(out, "\"kind\":\"auth_ok\"") != NULL);
    free_server(server, vm);
}

UTEST(dispatcher_auth_op_with_wrong_token_rejects)
{
    UVM *vm = (UVM *)calloc(1, sizeof(UVM));
    urbi_vm_init(vm, NULL, NULL);
    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port = -1;
    cfg.auth_token = "hunter2";
    int err = 0;
    UReplServer *server = urbi_repl_serve(vm, &cfg, &err);
    UReplSession *s = urepl_session_create(server);
    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    job->session_id = s->session_id;
    job->req.id = 5;
    job->req.op = UREPL_OP_AUTH;
    job->req.token = tdup("wrong");
    urepl_dispatch_job(server, job);
    UASSERT(s->authed == false);

    char out[256];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(strstr(out, "\"kind\":\"error\"") != NULL);
    UASSERT(strstr(out, "\"code\":\"auth_failed\"") != NULL);
    free_server(server, vm);
}

UTEST(dispatcher_preauth_eval_rejected_when_token_required)
{
    UVM *vm = (UVM *)calloc(1, sizeof(UVM));
    urbi_vm_init(vm, NULL, NULL);
    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port = -1;
    cfg.auth_token = "sekret";
    int err = 0;
    UReplServer *server = urbi_repl_serve(vm, &cfg, &err);
    UReplSession *s = urepl_session_create(server);
    UASSERT(s->authed == false);

    /* Attempt eval pre-auth. */
    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    job->session_id = s->session_id;
    job->req.id = 6;
    job->req.op = UREPL_OP_EVAL;
    job->req.code = tdup("1+2");
    job->req.code_len = 3;
    urepl_dispatch_job(server, job);

    char out[256];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(strstr(out, "\"code\":\"auth_required\"") != NULL);
    free_server(server, vm);
}

UTEST(dispatcher_unknown_session_dropped)
{
    /* Job for a session that doesn't exist must not crash and must
     * release the req strings. */
    UVM *vm = NULL;
    UReplServer *server = mk_server(&vm);

    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    job->session_id = 99999;
    job->req.id = 7;
    job->req.op = UREPL_OP_EVAL;
    job->req.code = tdup("1");
    job->req.code_len = 1;
    urepl_dispatch_job(server, job);  /* must not crash */
    free_server(server, vm);
}

UTEST(dispatcher_eval_id_tracking_round_trip)
{
    /* During an eval frame the session's current_eval_id is set;
     * a no-arg eval like "1+2" doesn't trigger session_writer (no
     * echo / print in the source), but we can still confirm the id
     * is reset to 0 after dispatch_job returns. */
    UVM *vm = NULL;
    UReplServer *server = mk_server(&vm);
    UReplSession *s = urepl_session_create(server);
    s->authed = true;
    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    job->session_id = s->session_id;
    job->req.id = 42;
    job->req.op = UREPL_OP_EVAL;
    job->req.code = tdup("1+2");
    job->req.code_len = 3;
    urepl_dispatch_job(server, job);
    UASSERT_EQ(s->current_eval_id, 0);
    free_server(server, vm);
}

UTEST(dispatcher_handles_introspect_coros)
{
    UVM *vm = NULL;
    UReplServer *server = mk_server(&vm);
    UReplSession *s = urepl_session_create(server);
    s->authed = true;

    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    job->session_id = s->session_id;
    job->req.id = 99;
    job->req.op = UREPL_OP_INTROSPECT;
    job->req.what = tdup("coros");
    urepl_dispatch_job(server, job);

    char out[4096];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(n > 0);
    UASSERT(strstr(out, "\"id\":99") != NULL);
    UASSERT(strstr(out, "\"kind\":\"result\"") != NULL);
    UASSERT(strstr(out, "\"coros\":") != NULL);
    free_server(server, vm);
}

UTEST(dispatcher_handles_introspect_gc)
{
    UVM *vm = NULL;
    UReplServer *server = mk_server(&vm);
    UReplSession *s = urepl_session_create(server);
    s->authed = true;

    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    job->session_id = s->session_id;
    job->req.id = 100;
    job->req.op = UREPL_OP_INTROSPECT;
    job->req.what = tdup("gc");
    urepl_dispatch_job(server, job);

    char out[4096];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(strstr(out, "\"id\":100") != NULL);
    UASSERT(strstr(out, "alive_bytes") != NULL);
    free_server(server, vm);
}

UTEST(dispatcher_introspect_unknown_what_emits_error)
{
    UVM *vm = NULL;
    UReplServer *server = mk_server(&vm);
    UReplSession *s = urepl_session_create(server);
    s->authed = true;

    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    job->session_id = s->session_id;
    job->req.id = 101;
    job->req.op = UREPL_OP_INTROSPECT;
    job->req.what = tdup("nonsense");
    urepl_dispatch_job(server, job);

    char out[1024];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(strstr(out, "\"code\":\"unknown_introspect\"") != NULL);
    free_server(server, vm);
}

/* ---- v0.9.1 Phase 5: Lobby session-lifecycle integration ------------- */

/* Helper: evaluate `expr` in the global realm and return the printed
 * result.  Used to peek at Lobby.lobbies.length() between dispatcher
 * mutations.  Returns the substring search for `needle` after eval. */
static int
eval_contains(UVM *vm, const char *expr, const char *needle)
{
    URealm *gr = urbi_realm_global(vm);
    char out[256];
    out[0] = '\0';
    int rc = urbi_repl_eval(vm, gr, expr, strlen(expr), out, sizeof(out));
    if (rc != URBI_OK) return 0;
    return strstr(out, needle) != NULL;
}

UTEST(dispatcher_session_create_grows_lobby_lobbies)
{
    UVM *vm = NULL;
    UReplServer *server = mk_server(&vm);
    UASSERT(server != NULL);

    /* Start: Lobby.lobbies is empty (lobby.u initialised it to List.new(),
     * and no REPL sessions have been created yet). */
    UASSERT(eval_contains(vm, "Lobby.lobbies.length()", "0"));

    UReplSession *s = urepl_session_create(server);
    UASSERT(s != NULL);

    /* After session_create: one entry. */
    UASSERT(eval_contains(vm, "Lobby.lobbies.length()", "1"));

    urepl_session_destroy(server, s);

    /* After session_destroy: empty again. */
    UASSERT(eval_contains(vm, "Lobby.lobbies.length()", "0"));

    free_server(server, vm);
}

UTEST(dispatcher_handleDisconnect_invoked_on_destroy)
{
    /* Validates that urepl_session_destroy invokes the C-side
     * urbi_lobby_invoke_handleDisconnect — observable through
     * mutation of the lobby instance itself.  The session's
     * Realm.global_object is a mutable cloned lobby, so the test
     * overrides handleDisconnect to write a sentinel back to Global
     * (Global is mutable; session-local writes survive the realm
     * teardown because Global is a VM singleton).
     *
     * This skips the cross-realm at-event-emit timing issue (the
     * watcher body fires asynchronously; getting it to drain in time
     * for a subsequent eval is brittle in unit-test scope).  The
     * direct slot-write check below verifies the invoke-hook end-to-
     * end without depending on the M5 reactive scheduler. */
    UVM *vm = NULL;
    UReplServer *server = mk_server(&vm);
    UASSERT(server != NULL);

    URealm *gr = urbi_realm_global(vm);
    char out[256];
    static const char INIT[] = "Global.disconnect_fired = 0";
    UASSERT_EQ(urbi_repl_eval(vm, gr, INIT, sizeof(INIT) - 1U,
                              out, sizeof(out)),
               URBI_OK);

    UReplSession *s = urepl_session_create(server);
    UASSERT(s != NULL);

    /* Install a per-session handleDisconnect override that bumps the
     * counter on Global.  Realm.handleDisconnect = function() { ... }
     * writes to the session realm's global_object (the lobby
     * instance), which is mutable. */
    static const char OVERRIDE[] =
        "Realm.handleDisconnect = "
        "function() { Global.disconnect_fired = Global.disconnect_fired + 1 }";
    UASSERT_EQ(urbi_repl_eval(vm, s->realm,
                              OVERRIDE, sizeof(OVERRIDE) - 1U,
                              out, sizeof(out)),
               URBI_OK);

    urepl_session_destroy(server, s);

    UASSERT(eval_contains(vm, "Global.disconnect_fired", "1"));

    free_server(server, vm);
}

UTEST(dispatcher_multiple_sessions_grow_and_shrink_lobby_lobbies)
{
    UVM *vm = NULL;
    UReplServer *server = mk_server(&vm);
    UASSERT(server != NULL);

    UReplSession *a = urepl_session_create(server);
    UReplSession *b = urepl_session_create(server);
    UReplSession *c = urepl_session_create(server);
    UASSERT(a && b && c);

    UASSERT(eval_contains(vm, "Lobby.lobbies.length()", "3"));

    urepl_session_destroy(server, b);
    UASSERT(eval_contains(vm, "Lobby.lobbies.length()", "2"));

    urepl_session_destroy(server, a);
    urepl_session_destroy(server, c);
    UASSERT(eval_contains(vm, "Lobby.lobbies.length()", "0"));

    free_server(server, vm);
}

/* Helper: create a mk_server + register one buffer transport + accept the
 * single cooperative session.  Caller owns server, vm, and bt_state. */
static UReplServer *
mk_server_with_buffer_transport(UVM **out_vm,
                                UBufferTransportState **out_bt)
{
    UVM *vm = (UVM *)calloc(1, sizeof(UVM));
    if (vm == NULL) return NULL;
    if (urbi_vm_init(vm, NULL, NULL) != URBI_OK) { free(vm); return NULL; }

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr = "127.0.0.1";
    cfg.tcp_port  = -1;

    int err = 0;
    UReplServer *server = urbi_repl_serve(vm, &cfg, &err);
    if (server == NULL) { urbi_vm_destroy(vm); free(vm); return NULL; }

    UBufferTransportState *bt = urepl_buffer_transport_create();
    if (bt == NULL) { urbi_repl_stop(server); urbi_vm_destroy(vm); free(vm); return NULL; }
    if (urbi_repl_register_transport(server, &UREPL_BUFFER_TRANSPORT, bt) != URBI_OK) {
        urepl_buffer_transport_destroy(bt);
        urbi_repl_stop(server); urbi_vm_destroy(vm); free(vm); return NULL;
    }

    /* Accept the session on the VM thread (buffer transport is non-pollable). */
    urepl_accept_sweep_nonpollable(server);

    *out_vm = vm;
    *out_bt = bt;
    return server;
}

/* REPL-07: a frame that parses successfully but has no `op` field
 * (op stays at UREPL_OP_NONE) must emit an `unknown_op` error envelope
 * correlated with the request id. */
UTEST(dispatcher_no_op_frame_emits_unknown_op_correlated)
{
    UVM *vm = NULL;
    UReplServer *server = mk_server(&vm);
    UASSERT(server != NULL);

    UReplSession *s = urepl_session_create(server);
    UASSERT(s != NULL);
    s->authed = true;

    /* Build a job with op=UREPL_OP_NONE (no op field parsed) and id=5. */
    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    UASSERT(job != NULL);
    job->session_id = s->session_id;
    job->req.id = 5;
    job->req.op = UREPL_OP_NONE;  /* simulates {"id":5} with no op key */

    urepl_dispatch_job(server, job);

    char out[512];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(n > 0);
    /* Must contain unknown_op code correlated with id 5. */
    UASSERT(strstr(out, "\"code\":\"unknown_op\"") != NULL);
    UASSERT(strstr(out, "\"id\":5") != NULL);

    free_server(server, vm);
}

/* REPL-05: an inbound line that exceeds the 8 KiB parse-buffer cap must
 * emit a `line_too_long` error envelope and then recover — the next valid
 * frame after the overlong one is parsed and dispatched normally. */
UTEST(dispatcher_inbound_line_too_long_emits_error_then_recovers)
{
    UVM *vm = NULL;
    UBufferTransportState *bt = NULL;
    UReplServer *server = mk_server_with_buffer_transport(&vm, &bt);
    UASSERT(server != NULL);

    /* Find the accepted session.  It is the head of sessions_head after
     * the accept sweep; authed since no token is configured. */
    UReplSession *s = server->sessions_head;
    UASSERT(s != NULL);

    /* Write 8200 bytes of non-newline data to the c2s ring.
     * First sweep reads 8192 bytes (fills the inbound buffer).
     * Second sweep: space==0 → overflow → line_too_long emitted, discard
     * mode set, remaining 8 bytes read and discarded. */
    char *junk = (char *)malloc(8200U);
    UASSERT(junk != NULL);
    memset(junk, 'x', 8200U);  /* no '\n' */
    urepl_buffer_client_write(bt, junk, 8200U);
    free(junk);

    /* First sweep: reads 8192 bytes, no error yet. */
    urepl_read_sweep_nonpollable(server);
    /* Second sweep: overflow detected — emits error, reads remaining 8 bytes,
     * discards (no newline found). */
    urepl_read_sweep_nonpollable(server);

    /* Drain the hello envelope + line_too_long envelope from the output. */
    char out[2048];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    /* The line_too_long envelope must be present. */
    UASSERT(strstr(out, "\"code\":\"line_too_long\"") != NULL);

    /* Now write the '\n' that ends the overlong line. */
    urepl_buffer_client_write(bt, "\n", 1U);
    urepl_read_sweep_nonpollable(server);  /* finds '\n', exits discard mode */

    /* Write a valid eval frame and verify it is processed correctly. */
    static const char eval_frame[] = "{\"id\":99,\"op\":\"eval\",\"code\":\"7+1\"}\n";
    urepl_buffer_client_write(bt, eval_frame, sizeof(eval_frame) - 1U);
    urepl_read_sweep_nonpollable(server);  /* parses + enqueues the eval job */
    urepl_dispatch_drain(server);          /* dispatches + writes result */

    /* Drain the output: must contain the eval result. */
    n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';
    UASSERT(strstr(out, "\"value\":\"8\"") != NULL);
    UASSERT(strstr(out, "\"id\":99") != NULL);

    urepl_buffer_transport_destroy(bt);
    urbi_repl_stop(server);
    urbi_vm_destroy(vm);
    free(vm);
}

void
test_repl_dispatcher_suite(void)
{
    printf("test_repl_dispatcher\n");
    utest_run("queue_init_destroy_empty",           queue_init_destroy_empty);
    utest_run("queue_push_drain_roundtrip",         queue_push_drain_roundtrip);
    utest_run("queue_drain_empty_returns_null",     queue_drain_empty_returns_null);
    utest_run("queue_destroy_frees_remaining_jobs", queue_destroy_frees_remaining_jobs);
    utest_run("queue_mpsc_stress_4_producers_100_each",
              queue_mpsc_stress_4_producers_100_each);
    utest_run("ringbuf_init_destroy_empty",         ringbuf_init_destroy_empty);
    utest_run("ringbuf_write_read_basic",           ringbuf_write_read_basic);
    utest_run("ringbuf_partial_read",               ringbuf_partial_read);
    utest_run("ringbuf_wraps_around",               ringbuf_wraps_around);
    utest_run("ringbuf_overflow_drops_oldest",      ringbuf_overflow_drops_oldest);
    utest_run("ringbuf_write_larger_than_cap",      ringbuf_write_larger_than_cap);
    utest_run("ringbuf_read_empty_returns_zero",    ringbuf_read_empty_returns_zero);
    utest_run("ringbuf_zero_size_init_rejects",     ringbuf_zero_size_init_rejects);
    utest_run("dispatcher_session_create_destroy",  dispatcher_session_create_destroy);
    utest_run("dispatcher_session_find_by_id",      dispatcher_session_find_by_id);
    utest_run("dispatcher_handles_eval_op",         dispatcher_handles_eval_op);
    utest_run("dispatcher_eval_compile_error_emits_error_envelope",
              dispatcher_eval_compile_error_emits_error_envelope);
    utest_run("dispatcher_auth_op_without_token_grants",
              dispatcher_auth_op_without_token_grants);
    utest_run("dispatcher_auth_op_with_correct_token",
              dispatcher_auth_op_with_correct_token);
    utest_run("dispatcher_auth_op_with_wrong_token_rejects",
              dispatcher_auth_op_with_wrong_token_rejects);
    utest_run("dispatcher_preauth_eval_rejected_when_token_required",
              dispatcher_preauth_eval_rejected_when_token_required);
    utest_run("dispatcher_unknown_session_dropped",
              dispatcher_unknown_session_dropped);
    utest_run("dispatcher_eval_id_tracking_round_trip",
              dispatcher_eval_id_tracking_round_trip);
    utest_run("dispatcher_handles_introspect_coros",
              dispatcher_handles_introspect_coros);
    utest_run("dispatcher_handles_introspect_gc",
              dispatcher_handles_introspect_gc);
    utest_run("dispatcher_introspect_unknown_what_emits_error",
              dispatcher_introspect_unknown_what_emits_error);
    utest_run("dispatcher_session_create_grows_lobby_lobbies",
              dispatcher_session_create_grows_lobby_lobbies);
    utest_run("dispatcher_multiple_sessions_grow_and_shrink_lobby_lobbies",
              dispatcher_multiple_sessions_grow_and_shrink_lobby_lobbies);
    utest_run("dispatcher_handleDisconnect_invoked_on_destroy",
              dispatcher_handleDisconnect_invoked_on_destroy);
    utest_run("dispatcher_no_op_frame_emits_unknown_op_correlated",
              dispatcher_no_op_frame_emits_unknown_op_correlated);
    utest_run("dispatcher_inbound_line_too_long_emits_error_then_recovers",
              dispatcher_inbound_line_too_long_emits_error_then_recovers);
}

#else  /* !URBI_ENABLE_REPL */

void test_repl_dispatcher_suite(void) { /* skipped: URBI_ENABLE_REPL=0 */ }

#endif
