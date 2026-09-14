/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_dispatch.h — session lifecycle and the op handlers.
 *
 * A session is a realm plus a stream.  Creating one allocates the realm,
 * points the realm's writer at the session so everything the session's
 * code echoes is framed for its own client and nobody else's, and applies
 * the server's compile budget.  Destroying one runs the lobby's
 * `handleDisconnect` and frees the realm, which unregisters it from
 * `Lobby.lobbies`. */

#ifndef UREPL_DISPATCH_H
#define UREPL_DISPATCH_H

#include "repl/urepl.h"
#include "repl/urepl_ndjson.h"

/* Allocates a session on `server` around an already-connected transport.
 * The transport struct is copied.  NULL on OOM, with nothing linked and
 * the transport neither adopted nor closed. */
UReplSession *urepl_session_create(UReplServer *server, const UTransport *transport);

/* Runs handleDisconnect, closes the transport, unlinks and frees.  The
 * realm goes with it. */
void urepl_session_destroy(UReplServer *server, UReplSession *s);

/* Appends one already-framed NDJSON line to the session's output.  A line
 * that does not fit is dropped and latched. */
void urepl_session_push(UReplSession *s, const char *bytes, size_t n);

/* Runs one parsed request and emits its response envelopes.  Takes the
 * request by pointer and does NOT free it; the caller owns it. */
void urepl_dispatch(UReplServer *server, UReplSession *s, const UReplNdjsonReq *req);

/* The three unsolicited error envelopes: a line that never parsed, a line
 * that exceeded the framing cap, and a gap where output was lost because
 * the client stopped reading.  All carry id 0 — none of them belongs to a
 * request the service could identify. */
void urepl_dispatch_parse_error(UReplSession *s);
void urepl_dispatch_line_too_long(UReplSession *s);
void urepl_dispatch_output_dropped(UReplSession *s);

#endif /* UREPL_DISPATCH_H */
