/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_dispatch.c - REPL job dispatcher + session machinery */
#include "repl/urepl_dispatch.h"
#include "repl/urepl_auth.h"
#include "repl/urepl_introspect.h"
#include "repl/urepl_listener.h"
#include "repl/urepl_ndjson.h"
#include "realm/urealm.h"
#include "vm/uvm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Default per-session output ringbuf cap (used when cfg.output_ringbuf_cap
 * is 0). */
#define UREPL_DEFAULT_OUTPUT_CAP (64U * 1024U)

/* ---- Helpers --------------------------------------------------------- */

/* Build a lobby id hex string from a 32-bit counter.  Format: 4-8 hex
 * digits zero-padded to at least 4.  Fits in lobby_id_hex[10]. */
static void
format_lobby_id(uint32_t id, char out[10])
{
    int n = snprintf(out, 10, "%04x", id);
    (void)n;  /* always 4 digits for 32-bit input up to 0xFFFF;
               * snprintf truncates safely for larger ids */
}

/* session_writer: realm-writer callback that captures urbiscript output
 * into the session's output ringbuf.  If the session is mid-eval
 * (current_eval_id != 0), the envelope carries that id; otherwise it
 * is lobby-scoped (no id, lobby field set). */
static void
session_writer(void *ud, const char *channel, size_t channel_len,
               const char *msg, size_t msg_len, uint64_t ts_us)
{
    UReplSession *s = (UReplSession *)ud;
    if (s == NULL) {
        return;
    }
    /* Channel must be NUL-terminated for emit; copy to a local stack
     * buffer.  The vm's writer fn typedef gives us length + pointer,
     * but channels are short identifiers (typically "clog" / "cerr"). */
    char channel_buf[64];
    if (channel_len >= sizeof(channel_buf)) {
        channel_len = sizeof(channel_buf) - 1;
    }
    if (channel != NULL && channel_len > 0U) {
        memcpy(channel_buf, channel, channel_len);
    }
    channel_buf[channel_len] = '\0';

    /* Stack-bounded envelope; spec §6 line cap is 1 MiB but typical
     * channel writes are O(100 B).  Truncate longer messages and emit
     * a single envelope; downstream tooling sees the truncation marker. */
    char env[4096];
    size_t n = 0;
    uint64_t id = s->current_eval_id;
    const char *lobby = (id == 0U) ? s->lobby_id_hex : NULL;
    if (msg_len > sizeof(env) / 2U) {
        /* Reserve room for envelope overhead. */
        msg_len = sizeof(env) / 2U;
    }
    int rc = urepl_ndjson_emit_output(env, sizeof(env), id, lobby,
                                      channel_buf, msg, msg_len,
                                      ts_us, &n);
    if (rc == 0) {
        urepl_ringbuf_write(&s->output, env, n);
    }
}

/* ---- Session lifecycle ----------------------------------------------- */

UReplSession *
urepl_session_create(UReplServer *server)
{
    if (server == NULL) {
        return NULL;
    }
    UReplSession *s = (UReplSession *)calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }

    /* Assign a unique session id + matching lobby hex. */
    pthread_mutex_lock(&server->sessions_mutex);
    s->session_id = server->next_session_id++;
    pthread_mutex_unlock(&server->sessions_mutex);
    format_lobby_id(s->session_id, s->lobby_id_hex);

    /* Sized output ringbuf. */
    size_t cap = server->cfg.output_ringbuf_cap;
    if (cap == 0U) {
        cap = UREPL_DEFAULT_OUTPUT_CAP;
    }
    if (urepl_ringbuf_init(&s->output, cap) != URBI_OK) {
        free(s);
        return NULL;
    }

    /* Per-session realm with REPL default compile-budget.  If the
     * server config overrode default_budget, apply that instead. */
    URealm *r = urbi_realm_create_repl(server->vm);
    if (r == NULL) {
        urepl_ringbuf_destroy(&s->output);
        free(s);
        return NULL;
    }
    bool override_budget = (server->cfg.default_budget.max_parser_depth != 0U
                            || server->cfg.default_budget.max_ast_nodes != 0U
                            || server->cfg.default_budget.max_source_bytes != 0U);
    if (override_budget) {
        urbi_realm_set_compile_budget(r, &server->cfg.default_budget);
    }
    s->vm = server->vm;
    s->realm = r;
    s->server = server;

    /* Install the session's writer so urbiscript output flows into our
     * ringbuf instead of the VM's default stderr writer. */
    urbi_realm_set_writer(server->vm, r, session_writer, s);

    /* Link into server's session list (head-insert). */
    pthread_mutex_lock(&server->sessions_mutex);
    s->next = server->sessions_head;
    server->sessions_head = s;
    pthread_mutex_unlock(&server->sessions_mutex);

    return s;
}

UReplSession *
urepl_session_find(UReplServer *server, uint32_t session_id)
{
    if (server == NULL) {
        return NULL;
    }
    pthread_mutex_lock(&server->sessions_mutex);
    UReplSession *s = server->sessions_head;
    while (s != NULL) {
        if (s->session_id == session_id) {
            break;
        }
        s = s->next;
    }
    pthread_mutex_unlock(&server->sessions_mutex);
    return s;
}

UReplSession *
urepl_session_find_by_lobby(UReplServer *server, const char *lobby_hex)
{
    if (server == NULL || lobby_hex == NULL) {
        return NULL;
    }
    pthread_mutex_lock(&server->sessions_mutex);
    UReplSession *s = server->sessions_head;
    while (s != NULL) {
        if (strcmp(s->lobby_id_hex, lobby_hex) == 0) {
            break;
        }
        s = s->next;
    }
    pthread_mutex_unlock(&server->sessions_mutex);
    return s;
}

void
urepl_session_destroy(UReplServer *server, UReplSession *session)
{
    if (server == NULL || session == NULL) {
        return;
    }
    /* Unlink from server's session list. */
    pthread_mutex_lock(&server->sessions_mutex);
    UReplSession **cur = &server->sessions_head;
    while (*cur != NULL) {
        if (*cur == session) {
            *cur = session->next;
            break;
        }
        cur = &(*cur)->next;
    }
    pthread_mutex_unlock(&server->sessions_mutex);

    /* Clear the realm's writer before destroying the realm to avoid a
     * dangling callback during teardown. */
    urbi_realm_set_writer(server->vm, session->realm, NULL, NULL);
    urbi_realm_destroy(server->vm, session->realm);
    urepl_ringbuf_destroy(&session->output);
    free(session);
}

/* ---- Op handlers ----------------------------------------------------- */

static void
push_env(UReplSession *s, const char *env, size_t n)
{
    urepl_ringbuf_write(&s->output, env, n);
}

static void
dispatch_eval(UReplServer *server, UReplSession *s, UReplJob *job)
{
    (void)server;
    char result[1024];
    /* Set live eval id BEFORE the eval so any session_writer hits
     * during the call carry the eval id (spec §6.3). */
    s->current_eval_id = job->req.id;

    int rc = urbi_repl_eval(s->vm, s->realm,
                            job->req.code,
                            job->req.code_len,
                            result, sizeof(result));

    /* Emit result or error envelope, then done.  Important: zero the
     * current_eval_id BEFORE the done envelope is emitted so any
     * post-done output (from watchers spawned by the eval) is
     * lobby-scoped per spec §6.3. */
    char env[4096];
    size_t n = 0;
    if (rc == URBI_OK) {
        /* Wrap result in JSON-string form.  urbi_repl_eval returns a
         * printable representation in 'result' (e.g. "3", "\"hello\"").
         * We treat it as a JSON string for now; Phase 4 Task 21 brings
         * a real JSON value formatter. */
        char value_json[1100];
        size_t off = 0;
        value_json[off++] = '"';
        int esc = urepl_json_escape(result, strlen(result),
                                    value_json + off,
                                    sizeof(value_json) - off - 2);
        if (esc < 0) {
            /* Fall back to a truncation marker. */
            const char *trunc = "<truncated>";
            size_t tn = strlen(trunc);
            memcpy(value_json + off, trunc, tn);
            off += tn;
        } else {
            off += (size_t)esc;
        }
        value_json[off++] = '"';
        value_json[off] = '\0';
        if (urepl_ndjson_emit_result(env, sizeof(env), job->req.id,
                                     value_json, 0, &n) == 0) {
            push_env(s, env, n);
        }
    } else {
        const char *code;
        switch (rc) {
        case URBI_ERR_COMPILE:                 code = "parse";        break;
        case URBI_ERR_STRAND_FATAL:            code = "runtime";      break;
        case URBI_ERR_COMPILE_BUDGET_DEPTH:    code = "budget_depth"; break;
        case URBI_ERR_COMPILE_BUDGET_NODES:    code = "budget_nodes"; break;
        case URBI_ERR_COMPILE_BUDGET_SOURCE:   code = "budget_source";break;
        case URBI_ERR_FROZEN_PROTO:            code = "frozen_proto"; break;
        case URBI_ERR_OOM:                     code = "oom";          break;
        default:                               code = "error";        break;
        }
        if (urepl_ndjson_emit_error(env, sizeof(env), job->req.id,
                                    code, result, &n) == 0) {
            push_env(s, env, n);
        }
    }

    /* Done envelope.  Zero the eval id BEFORE so any session_writer
     * fires after 'done' are lobby-scoped. */
    s->current_eval_id = 0U;
    if (urepl_ndjson_emit_done(env, sizeof(env), job->req.id, &n) == 0) {
        push_env(s, env, n);
    }
}

static void
dispatch_auth(UReplServer *server, UReplSession *s, UReplJob *job)
{
    char env[256];
    size_t n = 0;
    const char *expected = server->cfg.auth_token;
    if (expected == NULL || expected[0] == '\0') {
        /* No auth configured — treat as success (loopback default). */
        s->authed = true;
        if (urepl_ndjson_emit_auth_ok(env, sizeof(env), job->req.id, &n) == 0) {
            push_env(s, env, n);
        }
        return;
    }
    /* v0.9.1 Task 17: constant-time comparison.  strcmp's length-
     * dependent timing leaks ~1 byte per probe to an attacker timing
     * round-trips; urepl_auth_token_match walks the full token length
     * with a volatile accumulator (spec §7.3). */
    size_t token_len = (job->req.token != NULL) ? strlen(job->req.token) : 0U;
    size_t expected_len = strlen(expected);
    bool matched = urepl_auth_token_match(job->req.token, token_len,
                                          expected, expected_len);
    /* Task 18: bump the per-source rate-limiter on each result.  On a
     * successful auth the slot is cleared so a future legitimate
     * client doesn't inherit prior fail-count state. */
    if (server->auth_limiter != NULL) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now_us = (uint64_t)ts.tv_sec * 1000000ULL
                          + (uint64_t)ts.tv_nsec / 1000ULL;
        pthread_mutex_lock(&server->auth_limiter_mutex);
        if (matched) {
            urepl_auth_limiter_record_success(
                (UReplAuthLimiter *)server->auth_limiter, s->peer_id);
        } else {
            urepl_auth_limiter_record_fail(
                (UReplAuthLimiter *)server->auth_limiter,
                s->peer_id, now_us);
        }
        pthread_mutex_unlock(&server->auth_limiter_mutex);
    }
    if (matched) {
        s->authed = true;
        if (urepl_ndjson_emit_auth_ok(env, sizeof(env), job->req.id, &n) == 0) {
            push_env(s, env, n);
        }
    } else {
        if (urepl_ndjson_emit_error(env, sizeof(env), job->req.id,
                                    "auth_failed", NULL, &n) == 0) {
            push_env(s, env, n);
        }
    }
}

static void
dispatch_introspect(UReplServer *server, UReplSession *s, UReplJob *job)
{
    /* Each introspect_* primitive emits a structured JSON object into a
     * scratch buffer; we wrap it in a {kind:result,value:<inner>} envelope.
     *
     * Inner cap of 8 KiB matches the per-primitive expected ceiling for
     * idle / small VMs.  Buffer overflow returns an error envelope rather
     * than silently truncating the JSON. */
    char inner[8192];
    size_t inner_n = 0;
    const char *what = (job->req.what != NULL) ? job->req.what : "";
    int rc = -1;

    if      (strcmp(what, "coros")    == 0) rc = urbi_introspect_coros   (server->vm, inner, sizeof(inner), &inner_n);
    else if (strcmp(what, "tags")     == 0) rc = urbi_introspect_tags    (server->vm, inner, sizeof(inner), &inner_n);
    else if (strcmp(what, "watchers") == 0) rc = urbi_introspect_watchers(server->vm, inner, sizeof(inner), &inner_n);
    else if (strcmp(what, "events")   == 0) rc = urbi_introspect_events  (server->vm, inner, sizeof(inner), &inner_n);
    else if (strcmp(what, "profile")  == 0) rc = urbi_introspect_profile (server->vm, inner, sizeof(inner), &inner_n);
    else if (strcmp(what, "gc")       == 0) rc = urbi_introspect_gc      (server->vm, inner, sizeof(inner), &inner_n);
    else if (strcmp(what, "lobbies")  == 0) rc = urbi_introspect_lobbies (server->vm, inner, sizeof(inner), &inner_n);
    else if (strcmp(what, "stack")    == 0) rc = urbi_introspect_stack   (server->vm, job->req.coro_id, inner, sizeof(inner), &inner_n);
    else if (strcmp(what, "slots")    == 0) {
        const char *obj = (job->req.obj != NULL) ? job->req.obj : "";
        rc = urbi_introspect_slots(server->vm, s->realm,
                                   obj, strlen(obj),
                                   inner, sizeof(inner), &inner_n);
    } else {
        char env[256];
        size_t n = 0;
        if (urepl_ndjson_emit_error(env, sizeof(env), job->req.id,
                                    "unknown_introspect", what, &n) == 0) {
            push_env(s, env, n);
        }
        return;
    }

    if (rc != URBI_OK) {
        char env[256];
        size_t n = 0;
        if (urepl_ndjson_emit_error(env, sizeof(env), job->req.id,
                                    "introspect_failed", what, &n) == 0) {
            push_env(s, env, n);
        }
        return;
    }

    /* Wrap inner JSON in the result envelope.  inner is NOT NUL-terminated
     * by the introspect primitives, so we temporarily terminate it for
     * urepl_ndjson_emit_result (which expects a C string). */
    inner[inner_n] = '\0';
    char env[10240];
    size_t n = 0;
    if (urepl_ndjson_emit_result(env, sizeof(env), job->req.id,
                                 inner, 0, &n) == 0) {
        push_env(s, env, n);
    }
}

static void
dispatch_cancel_stub(UReplServer *server, UReplSession *s, UReplJob *job)
{
    /* Task 26 wires real tag.stop() lookup.  Stub: emit cancelled:0
     * (spec §6.6 "unknown tag is a benign no-op"). */
    (void)server;
    char env[256];
    size_t n = 0;
    if (urepl_ndjson_emit_result(env, sizeof(env), job->req.id,
                                 "{\"cancelled\":0}", 0, &n) == 0) {
        push_env(s, env, n);
    }
}

static void
dispatch_lobby_new(UReplServer *server, UReplSession *s, UReplJob *job)
{
    /* Multi-lobby per connection is deferred to v1.x.  For now, return
     * the existing session's lobby id (treat lobby_new as idempotent on
     * the implicit lobby).  Caller gets a result envelope with the lobby
     * field as a JSON string. */
    (void)server;
    char env[256];
    size_t n = 0;
    char value_json[32];
    snprintf(value_json, sizeof(value_json), "\"%s\"", s->lobby_id_hex);
    if (urepl_ndjson_emit_result(env, sizeof(env), job->req.id,
                                 value_json, 0, &n) == 0) {
        push_env(s, env, n);
    }
}

static void
dispatch_lobby_close(UReplServer *server, UReplSession *s, UReplJob *job)
{
    /* lobby_close on the implicit lobby is treated as a benign no-op in
     * v0.9.1 (the lobby is destroyed only when the connection closes).
     * Return result:true so clients can sequence shutdown. */
    (void)server;
    (void)s;
    char env[256];
    size_t n = 0;
    if (urepl_ndjson_emit_result(env, sizeof(env), job->req.id,
                                 "true", 0, &n) == 0) {
        push_env(s, env, n);
    }
}

/* ---- Job entry point ------------------------------------------------- */

void
urepl_dispatch_job(UReplServer *server, UReplJob *job)
{
    if (server == NULL || job == NULL) {
        return;
    }
    UReplSession *s = urepl_session_find(server, job->session_id);
    if (s == NULL) {
        /* Stale job for a closed session — drop silently. */
        urepl_ndjson_free_req(&job->req);
        free(job);
        return;
    }

    /* Pre-auth gate: only 'auth' is allowed before authed=true (spec §7). */
    if (!s->authed
        && server->cfg.auth_token != NULL
        && server->cfg.auth_token[0] != '\0'
        && job->req.op != UREPL_OP_AUTH) {
        char env[256];
        size_t n = 0;
        if (urepl_ndjson_emit_error(env, sizeof(env), job->req.id,
                                    "auth_required",
                                    "send {\"op\":\"auth\",\"token\":...} first",
                                    &n) == 0) {
            push_env(s, env, n);
        }
        urepl_ndjson_free_req(&job->req);
        free(job);
        return;
    }

    switch (job->req.op) {
    case UREPL_OP_AUTH:        dispatch_auth(server, s, job); break;
    case UREPL_OP_EVAL:        dispatch_eval(server, s, job); break;
    case UREPL_OP_CANCEL:      dispatch_cancel_stub(server, s, job); break;
    case UREPL_OP_INTROSPECT:  dispatch_introspect(server, s, job); break;
    case UREPL_OP_LOBBY_NEW:   dispatch_lobby_new(server, s, job); break;
    case UREPL_OP_LOBBY_CLOSE: dispatch_lobby_close(server, s, job); break;
    case UREPL_OP_NONE:
    default: {
        char env[128];
        size_t n = 0;
        if (urepl_ndjson_emit_error(env, sizeof(env), job->req.id,
                                    "unknown_op", NULL, &n) == 0) {
            push_env(s, env, n);
        }
        break;
    }
    }

    urepl_ndjson_free_req(&job->req);
    free(job);
}

void
urepl_dispatch_drain(UReplServer *server)
{
    if (server == NULL || server->job_queue == NULL) {
        return;
    }
    UReplJob *head = urepl_queue_drain_all(server->job_queue);
    while (head != NULL) {
        UReplJob *next = head->next;
        urepl_dispatch_job(server, head);
        head = next;
    }
}

/* Step-driver hook.  Called from urbi_step() before any opcode work.
 *
 * Order matters:
 *
 *   1. Drain the listener's pending-accept queue first.  Listener
 *      thread does no VM-touching work; new sessions get their realm
 *      bootstrapped here on the VM thread (spec §3.1).  Any session
 *      created in step 1 is then findable by sub-step 2/3.
 *
 *   2. Drain the job MPSC queue and dispatch each — writes response
 *      envelopes into per-session output ringbufs.
 *
 *   3. Wake every reader subthread so it flushes its session's
 *      ringbuf to socket.
 *
 * Linked weakly from src/vm/ustep.c so the default (URBI_ENABLE_REPL=0)
 * build resolves to a no-op without dragging the REPL TUs in. */
void
urepl_dispatch_drain_if_active(struct UVM *vm)
{
    if (vm == NULL || vm->repl_server == NULL) {
        return;
    }
    UReplServer *server = (UReplServer *)vm->repl_server;
    urepl_listener_drain_accepts(server);
    urepl_dispatch_drain(server);
    urepl_listener_wake_all_readers(server);
}
