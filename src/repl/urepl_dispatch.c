/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_dispatch.c — see repl/urepl_dispatch.h. */

#include "repl/urepl_dispatch.h"

#include "rt/urealm.h"
#include "stdlib/debug_namespace.h"   /* urbi_introspect_coros */

#include <stdlib.h>
#include <string.h>

/* Default per-session output staging when the config leaves it at zero. */
#define UREPL_DEFAULT_OUTPUT_CAP ((size_t)64U * 1024U)

/* ---- output staging --------------------------------------------------- */

/* Reclaims the leading `off` bytes the transport has already taken.  Done
 * lazily, at the point a write would not otherwise fit, so a session that
 * drains completely every sweep never memmoves at all. */
static void outbuf_compact(UReplOutBuf *o)
{
    if (o->buf == NULL || o->off == 0) return;
    if (o->off >= o->fill) { o->fill = 0; o->off = 0; return; }
    memmove(o->buf, o->buf + o->off, o->fill - o->off);
    o->fill -= o->off;
    o->off = 0;
}

/* Grows the buffer to hold `need` more bytes, doubling from 256 up to the
 * session's cap.  false means the cap cannot hold it, or the allocation
 * failed; either way the caller reports the write dropped. */
static bool outbuf_reserve(UReplOutBuf *o, size_t need)
{
    if (o->fill + need <= o->cap) return true;
    size_t cap = o->cap ? o->cap : 256u;
    while (cap < o->fill + need && cap < o->cap_limit) cap *= 2u;
    if (cap > o->cap_limit) cap = o->cap_limit;
    if (o->fill + need > cap) return false;
    char *n = (char *)realloc(o->buf, cap);
    if (n == NULL) return false;
    o->buf = n;
    o->cap = cap;
    return true;
}

void urepl_session_push(UReplSession *s, const char *bytes, size_t n)
{
    UReplOutBuf *o = &s->out;
    if (o->fill + n > o->cap) outbuf_compact(o);
    if (!outbuf_reserve(o, n)) { o->dropped = true; return; }
    memcpy(o->buf + o->fill, bytes, n);
    o->fill += n;
}

/* The two shapes every handler ends in.  A local buffer rather than a
 * shared one: these are re-entered through the session writer while an
 * eval is running, and a shared scratch buffer would be clobbered
 * mid-frame. */
static void push_error(UReplSession *s, uint64_t id, const char *code, const char *msg)
{
    char env[1024];
    size_t n = 0;
    if (urepl_ndjson_emit_error(env, sizeof env, id, code, msg, &n) == 0)
        urepl_session_push(s, env, n);
}

static void push_result(UReplSession *s, uint64_t id, const char *value_json)
{
    char env[8192];
    size_t n = 0;
    if (urepl_ndjson_emit_result(env, sizeof env, id, value_json, &n) == 0)
        urepl_session_push(s, env, n);
    else
        push_error(s, id, "response_too_large", NULL);
}

void urepl_dispatch_parse_error(UReplSession *s)
{
    /* Id zero: the line never parsed, so there is no id to correlate to. */
    push_error(s, 0, "parse", "malformed request");
}

void urepl_dispatch_line_too_long(UReplSession *s)
{
    push_error(s, 0, "line_too_long", "request exceeded the framing cap");
}

void urepl_dispatch_output_dropped(UReplSession *s)
{
    push_error(s, 0, "output_dropped", "output was lost: the client is not reading");
}

/* ---- the session writer ------------------------------------------------
 *
 * Installed on the session's realm, so everything the session's own code
 * echoes is framed as an output envelope for the session's own client.
 * Output produced INSIDE an eval carries that eval's id; output produced
 * after its `done` — by a watcher or a timer the eval armed — carries
 * none, which is how a client tells a reply from an interruption. */
static void session_writer(void *ud, const char *chan, size_t cl,
                           const char *msg, size_t ml)
{
    UReplSession *s = (UReplSession *)ud;
    if (s == NULL) return;

    char channel[64];
    if (cl >= sizeof channel) cl = sizeof channel - 1;
    if (chan != NULL && cl > 0) memcpy(channel, chan, cl);
    channel[cl] = '\0';

    /* Half the envelope is reserved for framing and escapes, so a
     * message longer than that is truncated rather than dropped: a
     * shortened line of tracing is more use than none. */
    char env[4096];
    if (ml > sizeof env / 2) ml = sizeof env / 2;
    size_t n = 0;
    if (urepl_ndjson_emit_output(env, sizeof env, s->current_eval_id,
                                 channel, msg, ml, &n) == 0)
        urepl_session_push(s, env, n);
    else
        s->out.dropped = true;
}

/* ---- session lifecycle ------------------------------------------------- */

UReplSession *urepl_session_create(UReplServer *server, const UTransport *transport)
{
    if (server == NULL || transport == NULL) return NULL;

    UReplSession *s = (UReplSession *)calloc(1, sizeof *s);
    if (s == NULL) return NULL;

    /* The output buffer is made on the first write and grows to the cap;
     * a session that never speaks costs nothing, which is what a part
     * with a few sessions and a few hundred kilobytes needs. */
    s->out.cap_limit = server->cfg.output_buf_cap ? server->cfg.output_buf_cap
                                                  : UREPL_DEFAULT_OUTPUT_CAP;

    /* One realm per session is the whole isolation story: its globals
     * object is its own, and the built-ins below it are shared. */
    s->realm = urbi_realm_new(server->vm);
    if (s->realm == NULL) { free(s); return NULL; }

    s->vm = server->vm;
    s->transport = *transport;
    urealm_set_writer(server->vm, s->realm, session_writer, s);
    urealm_set_budget(server->vm, s->realm, &server->cfg.default_budget);

    s->next = server->sessions;
    server->sessions = s;
    return s;
}

void urepl_session_destroy(UReplServer *server, UReplSession *s)
{
    if (server == NULL || s == NULL) return;

    /* The lobby's disconnect hook, before the realm goes.  It is a script
     * call because the hook is a script slot the client may have
     * replaced; whatever it throws is dropped, since a teardown that
     * fails is still a teardown. */
    if (s->realm != NULL) {
        static const char HOOK[] = "Realm.handleDisconnect()";
        UValue ignored;
        char err[128];
        (void)urbi_run(server->vm, s->realm, HOOK, sizeof HOOK - 1u,
                       "<disconnect>", &ignored, err, sizeof err);
        /* The hook's own failure must not look like the VM's. */
        urbi_clear_error(server->vm);
    }

    /* Detach the writer before the realm is freed: a strand of this realm
     * still on the run queue would otherwise write into a freed session. */
    if (s->realm != NULL) urealm_set_writer(server->vm, s->realm, NULL, NULL);
    if (s->realm != NULL) urbi_realm_free(server->vm, s->realm);

    if (!s->closed && s->transport.close != NULL) s->transport.close(s->transport.ctx);
    s->closed = true;

    for (UReplSession **pp = &server->sessions; *pp; pp = &(*pp)->next) {
        if (*pp == s) { *pp = s->next; break; }
    }
    free(s->out.buf);
    free(s->in);
    free(s);
}

/* ---- op handlers ------------------------------------------------------- */

/* The service's error vocabulary.  Each name is what a client keys on, so
 * the mapping is the wire contract, not a convenience. */
static const char *error_code_for(int rc)
{
    switch (rc) {
    case URBI_ERR_COMPILE:               return "parse";
    case URBI_ERR_COMPILE_BUDGET_DEPTH:  return "budget_depth";
    case URBI_ERR_COMPILE_BUDGET_NODES:  return "budget_nodes";
    case URBI_ERR_COMPILE_BUDGET_SOURCE: return "budget_source";
    case URBI_ERR_UNCAUGHT_THROW:        return "runtime";
    case URBI_ERR_OOM:                   return "oom";
    default:                             return "error";
    }
}

static void dispatch_eval(UReplServer *server, UReplSession *s, const UReplNdjsonReq *req)
{
    if (req->code == NULL) { push_error(s, req->id, "parse", "eval without code"); return; }

    char err[512];
    err[0] = '\0';
    UValue value = urbi_make_nil();

    /* Set before the run, cleared before the `done`, so the window in
     * which output is attributed to this eval is exactly the eval. */
    s->current_eval_id = req->id;
    int rc = urbi_run(server->vm, s->realm, req->code, req->code_len,
                      "<stdin>", &value, err, sizeof err);

    if (rc == URBI_OK) {
        /* The value goes over as a JSON string holding what the REPL would
         * have printed.  A structural JSON rendering of an arbitrary
         * urbiscript value is a separate design, not a formatting detail. */
        char rendered[1024];
        size_t rn = urbi_value_to_string(server->vm, value, rendered, sizeof rendered);
        char quoted[2200];
        quoted[0] = '"';
        int esc = urepl_json_escape(rendered, rn, quoted + 1, sizeof quoted - 3);
        if (esc < 0) { push_error(s, req->id, "response_too_large", NULL); }
        else {
            quoted[1 + (size_t)esc] = '"';
            quoted[2 + (size_t)esc] = '\0';
            push_result(s, req->id, quoted);
        }
    } else {
        /* A compile failure explains itself in `err`; a throw explains
         * itself through the error channel. */
        const char *msg = err;
        if (msg[0] == '\0') {
            UErrorInfo info;
            (void)urbi_last_error(server->vm, &info);
            msg = (info.message != NULL && info.message[0] != '\0') ? info.message : "";
        }
        push_error(s, req->id, error_code_for(rc), msg);
        urbi_clear_error(server->vm);
    }

    s->current_eval_id = 0;
    char env[128];
    size_t n = 0;
    if (urepl_ndjson_emit_done(env, sizeof env, req->id, &n) == 0)
        urepl_session_push(s, env, n);
}

static void dispatch_introspect(UReplServer *server, UReplSession *s,
                                const UReplNdjsonReq *req)
{
    const char *what = req->what != NULL ? req->what : "";
    if (strcmp(what, "coros") != 0) {
        /* The eight other primitives the old server carried each walked a
         * structure the re-founded runtime does not have in that shape;
         * they return with the server, rather than as eight stubs. */
        push_error(s, req->id, "unknown_introspect", what);
        return;
    }
    char inner[4096];
    size_t n = 0;
    if (urbi_introspect_coros(server->vm, inner, sizeof inner, &n) != URBI_OK) {
        push_error(s, req->id, "introspect_failed", what);
        return;
    }
    inner[n] = '\0';
    /* Inline, not quoted: an introspect answer IS JSON already. */
    push_result(s, req->id, inner);
}

void urepl_dispatch(UReplServer *server, UReplSession *s, const UReplNdjsonReq *req)
{
    if (server == NULL || s == NULL || req == NULL) return;
    switch (req->op) {
    case UREPL_OP_EVAL:       dispatch_eval(server, s, req); break;
    case UREPL_OP_INTROSPECT: dispatch_introspect(server, s, req); break;
    default:                  push_error(s, req->id, "unknown_op", NULL); break;
    }
}
