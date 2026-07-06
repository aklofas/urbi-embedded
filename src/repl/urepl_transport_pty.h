/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_transport_pty.h - Linux pty-pair transport (v0.9.1)
 *
 * Test-only harness driving the listener thread end-to-end with no real
 * socket and no UART hardware.  openpty() allocates a (master, slave) fd
 * pair; the slave behaves as the REPL service's "client" fd, while the
 * test driver writes/reads on the master side to play the role of a
 * remote peer.
 *
 * Single-client, mirroring spec §10.2 — accept_fn returns 0 once with the
 * pre-opened slave fd, then returns -1 (would-block) forever after.  This
 * matches the real UART pattern: the wire is already established at
 * register time; there is no "listen" step.  The listener thread treats
 * the slave fd as the pollable listener fd so a `read` on the master
 * side wakes poll() promptly.
 *
 * Only compiled when URBI_ENABLE_REPL=1.  Linux-only (depends on glibc
 * <pty.h>); real-hardware UART drivers are in urepl_transport_uart_*.c. */
#ifndef UREPL_TRANSPORT_PTY_H
#define UREPL_TRANSPORT_PTY_H

#include "urbi/repl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct UPtyState UPtyState;

/* Allocate a (master, slave) pty pair via openpty().  The slave fd is
 * configured non-blocking so the transport's read_fn cleanly returns -1
 * on empty drain.  Returns NULL on openpty() failure. */
UPtyState *urepl_pty_state_create(void);

/* Close both fds + free the state.  Idempotent on NULL. */
void       urepl_pty_state_destroy(UPtyState *st);

/* Test-side accessor for the master fd.  This is what the driving test
 * writes NDJSON requests to and reads response envelopes from. */
int        urepl_pty_master_fd(const UPtyState *st);

/* Listener-side accessor for the slave fd.  Used by the REPL listener
 * thread (urepl_listener.c) as the pollable "listen fd" so master-side
 * writes wake the accept loop promptly.  After accept_fn has handed
 * the slave fd to the reader subthread it's still the same fd; the
 * listener's poll for POLLIN simply becomes a no-op since accept_fn
 * returns -1 thereafter (single-client). */
int        urepl_pty_slave_fd(const UPtyState *st);

/* The transport vtable.  Pass &UREPL_PTY_TRANSPORT (plus the UPtyState
 * pointer) to urbi_repl_register_transport. */
extern const UTransport UREPL_PTY_TRANSPORT;

#ifdef __cplusplus
}
#endif

#endif /* UREPL_TRANSPORT_PTY_H */
