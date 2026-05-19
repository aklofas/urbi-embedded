/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl.h - internal REPL service types (v0.9.1)
 *
 * Only compiled when URBI_ENABLE_REPL=1.  Public REPL types live in
 * <urbi/repl.h>; this header pulls those in and extends with internal-
 * only types (sessions, transport list, etc.). */
#ifndef SRC_REPL_UREPL_H
#define SRC_REPL_UREPL_H

#include "urbi/repl.h"
#include "urbi/urbi.h"

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations — full definitions in private TUs. */
typedef struct UReplSession UReplSession;
typedef struct UReplQueue UReplQueue;
typedef struct UReplRingbuf UReplRingbuf;
typedef struct UReplJob UReplJob;
typedef struct UReplTransportEntry UReplTransportEntry;

/* Transport list entry.  Each call to urbi_repl_register_transport
 * appends one of these to the server's transport chain. */
struct UReplTransportEntry {
    const UTransport          *transport;
    void                      *listener_state;
    struct UReplTransportEntry *next;
};

/* The server is opaque to embedders; this is the full layout used by
 * src/repl/* TUs. */
struct UReplServer {
    struct UVM              *vm;
    UReplConfig              cfg;
    UReplTransportEntry     *transports;
    UReplQueue              *job_queue;
    UReplSession            *sessions_head;
    uint32_t                 next_session_id;
    pthread_mutex_t          sessions_mutex;
    bool                     shutting_down;
};

#endif /* SRC_REPL_UREPL_H */
