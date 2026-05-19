/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl.c - REPL server lifecycle (v0.9.1)
 *
 * Phase 2 ships the create/destroy + default-secure check + transport
 * registration.  The listener thread + per-connection reader thread come
 * online in Phase 3 (Task 16). */
#include "repl/urepl.h"
#include "repl/urepl_queue.h"
#include "repl/urepl_dispatch.h"

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
    if (pthread_mutex_init(&server->sessions_mutex, NULL) != 0) {
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
        pthread_mutex_destroy(&server->sessions_mutex);
        free(server);
        if (out_err != NULL) {
            *out_err = URBI_ERR_OOM;
        }
        return NULL;
    }
    return server;
}

void
urbi_repl_stop(UReplServer *server)
{
    if (server == NULL) {
        return;
    }
    server->shutting_down = true;

    /* Tear down all live sessions before destroying the realm-borrowing
     * VM.  Each session_destroy unlinks itself from the head list. */
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
    pthread_mutex_destroy(&server->sessions_mutex);
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

int
urbi_repl_serve_step(UReplServer *server, uint64_t timeout_us)
{
    (void)timeout_us;
    if (server == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    /* Phase 3 connects this to the accept/read/dispatch/write cycle.
     * In Phase 2 it is a no-op success. */
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
    return URBI_OK;
}
