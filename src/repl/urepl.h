/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl.h — the shapes behind <urbi/repl.h>.
 *
 * One server, a list of sessions, and nothing else: no queue, no mutex,
 * no thread.  Everything here runs on the caller's thread inside
 * urbi_repl_serve_step, which is the whole reason the cooperative core
 * could come back before the networked server did.
 *
 * The networked server's session machinery — auth state, peer identity,
 * rate-limit counters, the reader thread back-pointer — is not reduced
 * here, it is absent.  Each of those fields existed to coordinate with a
 * thread that no longer runs. */

#ifndef UREPL_H
#define UREPL_H

#include "urbi/repl.h"
#include "urbi/urbi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct UReplSession;
typedef struct UReplSession UReplSession;

/* Per-session output staging.  A plain FIFO of bytes, not a ring: the
 * consumer is the transport in the same sweep that produced them, so the
 * only thing wraparound would buy is a memmove saved on a buffer that is
 * usually empty.
 *
 * `buf` is NULL until the first write, then doubles from 256 bytes as
 * output arrives, never past `cap_limit` (the server config's
 * output_buf_cap, 64 KiB when that is zero).  A session that never
 * speaks holds no buffer at all.
 *
 * An envelope that does not fit is DROPPED, and `dropped` latches so the
 * client is told a gap happened rather than silently receiving a
 * truncated stream.  Dropping beats blocking: the alternative is stalling
 * the VM on a client that has stopped reading. */
typedef struct UReplOutBuf {
    char   *buf;
    size_t  cap;      /* allocated; 0 until the first write */
    size_t  cap_limit; /* never grow past this */
    size_t  fill;     /* bytes held */
    size_t  off;      /* bytes already handed to the transport */
    bool    dropped;  /* an envelope was lost; report once, then clear */
} UReplOutBuf;

struct UReplSession {
    UVM         *vm;
    URealm      *realm;
    UTransport   transport;
    UReplOutBuf  out;

    /* Inbound line accumulator.  A transport hands over arbitrary chunks,
     * so a request may arrive in pieces and two may arrive at once. */
    char   *in;
    size_t  in_cap;
    size_t  in_fill;
    /* Set when a line exceeded the framing cap: bytes are discarded until
     * the newline that ends it, so one oversized request cannot desync
     * every request after it. */
    bool    in_discard;

    /* The id of the eval currently running, so output written inside the
     * frame is correlated to it.  Zero outside an eval, which is what
     * marks a watcher's output as unsolicited. */
    uint64_t current_eval_id;

    bool     ended;      /* the transport reported end of stream */
    bool     closed;     /* close() has been called; do not call it twice */

    struct UReplSession *next;
};

struct UReplServer {
    UVM          *vm;
    UReplConfig   cfg;
    UReplSession *sessions;
};

#endif /* UREPL_H */
