/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl.c - REPL server lifecycle (v0.9.1)
 *
 * Phase 2 ships the create/destroy + default-secure check + transport
 * registration.  The listener thread + per-connection reader thread come
 * online in Phase 3 (Task 16). */
#include "repl/urepl.h"
#include "repl/urepl_auth.h"
#include "repl/urepl_dispatch.h"
#include "repl/urepl_listener.h"
#include "repl/urepl_queue.h"
#include "vm/uvm.h"

#include <stdlib.h>
#include <string.h>

/* Default-secure rule: if bind_addr is non-loopback (i.e. neither NULL,
 * "127.0.0.1", "::1", nor a Unix-socket path beginning with '/'), an
 * auth_token must be set. */
static bool
is_loopback_bind(const UReplConfig *cfg)
{
    if (cfg->bind_addr == NULL) {
        return true;  /* NULL defaults to loopback */
    }
    if (cfg->bind_addr[0] == '\0') {
        return true;
    }
    if (cfg->bind_addr[0] == '/') {
        return true;  /* Unix-domain socket path */
    }
    if (strcmp(cfg->bind_addr, "127.0.0.1") == 0) {
        return true;
    }
    if (strcmp(cfg->bind_addr, "::1") == 0) {
        return true;
    }
    if (strcmp(cfg->bind_addr, "localhost") == 0) {
        return true;
    }
    return false;
}

UReplServer *
urbi_repl_serve(struct UVM *vm, const UReplConfig *cfg, int *out_err)
{
    if (out_err != NULL) {
        *out_err = URBI_OK;
    }
    if (vm == NULL || cfg == NULL) {
        if (out_err != NULL) {
            *out_err = URBI_ERR_INVALID_ARG;
        }
        return NULL;
    }

    if (!is_loopback_bind(cfg) && cfg->auth_token == NULL) {
        if (out_err != NULL) {
            *out_err = URBI_ERR_INSECURE_CONFIG;
        }
        return NULL;
    }

    UReplServer *server = (UReplServer *)calloc(1, sizeof(*server));
    if (server == NULL) {
        if (out_err != NULL) {
            *out_err = URBI_ERR_OOM;
        }
        return NULL;
    }
    server->vm = vm;
    server->cfg = *cfg;
    server->next_session_id = 1U;
    server->stop_eventfd = -1;
    if (UREPL_MUTEX_INIT(&server->sessions_mutex) != 0) {
        free(server);
        if (out_err != NULL) {
            *out_err = URBI_ERR_OOM;
        }
        return NULL;
    }
    if (UREPL_MUTEX_INIT(&server->auth_limiter_mutex) != 0) {
        UREPL_MUTEX_DESTROY(&server->sessions_mutex);
        free(server);
        if (out_err != NULL) {
            *out_err = URBI_ERR_OOM;
        }
        return NULL;
    }
    if (UREPL_MUTEX_INIT(&server->accept_queue_mutex) != 0) {
        UREPL_MUTEX_DESTROY(&server->auth_limiter_mutex);
        UREPL_MUTEX_DESTROY(&server->sessions_mutex);
        free(server);
        if (out_err != NULL) {
            *out_err = URBI_ERR_OOM;
        }
        return NULL;
    }
    /* Allocate the per-server job queue.  Phase 3 hooks the listener
     * thread up to it; in Phase 2 it is used by direct callers to
     * urepl_dispatch_drain for unit tests. */
    server->job_queue = (UReplQueue *)calloc(1, sizeof(*server->job_queue));
    if (server->job_queue == NULL
        || urepl_queue_init(server->job_queue) != URBI_OK) {
        free(server->job_queue);
        UREPL_MUTEX_DESTROY(&server->accept_queue_mutex);
        UREPL_MUTEX_DESTROY(&server->auth_limiter_mutex);
        UREPL_MUTEX_DESTROY(&server->sessions_mutex);
        free(server);
        if (out_err != NULL) {
            *out_err = URBI_ERR_OOM;
        }
        return NULL;
    }

    /* Task 18: spin up per-IP rate limiter iff auth is enabled.  Loop-
     * back no-auth deployments skip it (no wrong-token attempts to
     * count).  Default tunables: 5 fails / 30 s window / 60 s lockout
     * (spec §7.4). */
    if (cfg->auth_token != NULL && cfg->auth_token[0] != '\0') {
        UReplAuthLimiter *lim =
            (UReplAuthLimiter *)calloc(1, sizeof(*lim));
        if (lim == NULL) {
            urepl_queue_destroy(server->job_queue);
            free(server->job_queue);
            UREPL_MUTEX_DESTROY(&server->accept_queue_mutex);
            UREPL_MUTEX_DESTROY(&server->auth_limiter_mutex);
            UREPL_MUTEX_DESTROY(&server->sessions_mutex);
            free(server);
            if (out_err != NULL) {
                *out_err = URBI_ERR_OOM;
            }
            return NULL;
        }
        urepl_auth_limiter_init(lim);
        server->auth_limiter = lim;
    }

    /* Register the server on the VM so urepl_dispatch_drain_if_active
     * (the step-driver hook) finds it without a global lookup table. */
    vm->repl_server = server;

    return server;
}

void
urbi_repl_stop(UReplServer *server)
{
    if (server == NULL) {
        return;
    }
    UREPL_ATOMIC_STORE_BOOL(&server->shutting_down, true);

    /* Phase 3: signal + join the listener pthread and all reader
     * subthreads BEFORE tearing down sessions/queue/vm.  Reader threads
     * destroy their sessions on the way out (see reader_main), so by
     * the time this returns sessions_head is typically empty.  Any
     * still-attached session (e.g. unit tests that created sessions
     * directly) gets reaped in the loop below. */
    urepl_listener_stop_and_join(server);

    /* Reap any unit-test sessions that weren't owned by a reader. */
    while (server->sessions_head != NULL) {
        urepl_session_destroy(server, server->sessions_head);
    }

    /* Drain + free the job queue. */
    if (server->job_queue != NULL) {
        urepl_queue_signal_shutdown(server->job_queue);
        urepl_queue_destroy(server->job_queue);
        free(server->job_queue);
        server->job_queue = NULL;
    }

    /* Free transport-list entries.  Listener-state is owned by the
     * caller of urbi_repl_register_transport. */
    UReplTransportEntry *e = server->transports;
    while (e != NULL) {
        UReplTransportEntry *next = e->next;
        free(e);
        e = next;
    }
    server->transports = NULL;

    /* auth_limiter struct is allocated by Task 18 (urepl_auth.c) if a
     * token is configured; free if present. */
    if (server->auth_limiter != NULL) {
        free(server->auth_limiter);
        server->auth_limiter = NULL;
    }
    UREPL_MUTEX_DESTROY(&server->auth_limiter_mutex);

    /* Drain + free any pending-accept items that the listener pushed
     * after the last VM-thread drain but before shutdown.  Each item
     * owns its client_fd; close + free. */
    UReplAcceptItem *ai = server->accept_head;
    server->accept_head = NULL;
    server->accept_tail = NULL;
    while (ai != NULL) {
        UReplAcceptItem *anext = ai->next;
        if (ai->transport != NULL && ai->transport->close_fn != NULL) {
            ai->transport->close_fn(ai->client_fd);
        }
        free(ai);
        ai = anext;
    }
    UREPL_MUTEX_DESTROY(&server->accept_queue_mutex);

    /* Unhook the VM back-pointer so the step-driver drain hook no
     * longer sees a freed server. */
    if (server->vm != NULL && server->vm->repl_server == server) {
        server->vm->repl_server = NULL;
    }

    UREPL_MUTEX_DESTROY(&server->sessions_mutex);
    free(server);
}

int
urbi_repl_serve_init(struct UVM *vm, const UReplConfig *cfg, UReplServer **out_server)
{
    int err = URBI_OK;
    if (out_server == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    *out_server = urbi_repl_serve(vm, cfg, &err);
    return err;
}

/* Public API — signature pinned by include/urbi/repl.h.  Cooperative
 * data plane for non-pollable transports (Pico USB CDC, UART).  The
 * embedder is expected to call this periodically (e.g. between
 * urbi_step iterations) and to __wfi() / sleep when idle. */
int
urbi_repl_serve_step(UReplServer *server, uint64_t timeout_us)
{
    (void)timeout_us;  /* Best-effort non-blocking sweep; no internal
                          wait — caller paces idle. */
    if (server == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    /* Phase A: accept new clients on non-pollable transports.  Pollable
     * transports stay on the listener pthread (when running). */
    (void)urepl_accept_sweep_nonpollable(server);
    /* Phase B: read NDJSON bytes from each non-pollable session and
     * push complete lines as jobs onto server->job_queue. */
    (void)urepl_read_sweep_nonpollable(server);
    /* Drain the job queue on this (VM) thread so the embedder doesn't
     * also have to drive urbi_step just to get dispatch.  This is the
     * cooperative counterpart of urepl_dispatch_drain_if_active's
     * step-hook invocation. */
    urepl_dispatch_drain(server);
    /* Phase C: drain pending output from each non-pollable session's
     * ringbuf via one non-blocking write_fn call per session.  Partial
     * writes stage in per-session coop_outbuf for the next sweep. */
    (void)urepl_write_sweep_nonpollable(server);
    /* Phase D: reap sessions whose read or write sweep set
     * needs_teardown (clean EOF, hard transport error).  Calls close_fn,
     * pthread_joins + frees the paired reader, fires the v0.9.1
     * disconnect-cleanup sequence, and unlinks from sessions_head. */
    (void)urepl_disconnect_sweep(server);
    return URBI_OK;
}

void
urbi_repl_serve_shutdown(UReplServer *server)
{
    urbi_repl_stop(server);
}

int
urbi_repl_register_transport(UReplServer *server,
                             const UTransport *transport,
                             void *listener_state)
{
    if (server == NULL || transport == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    UReplTransportEntry *entry = (UReplTransportEntry *)calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return URBI_ERR_OOM;
    }
    entry->transport = transport;
    entry->listener_state = listener_state;
    /* Append to head — order does not matter (each transport runs its
     * own accept loop in Phase 3). */
    entry->next = server->transports;
    server->transports = entry;

    /* Phase 3: lazily start the listener pthread on first transport
     * registration.  Embedders that only use the in-process buffer
     * transport (unit tests) drive the dispatcher manually; the
     * listener thread no-ops on buffer transport (its pollable
     * listener fd is -1) so starting it here is harmless even for
     * the test path.  Tests can opt out of starting the listener by
     * never calling urbi_repl_register_transport — the dispatcher
     * tests in Phase 2 use that path. */
    (void)urepl_listener_start(server);

    return URBI_OK;
}
