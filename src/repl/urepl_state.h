/* SPDX-License-Identifier: BSD-3-Clause */
/* === W3/v0.10.4: REPL substate extracted from struct UVM per audit-1 F8. ===
 *
 * Currently a single back-pointer to the heap-allocated UReplServer; the
 * structured form is in place for future expansion.
 *
 * Lifecycle:
 *   Allocated by urbi_repl_serve (src/repl/urepl.c) when a server is started.
 *   Freed by urbi_repl_stop.  vm->repl is NULL when no server is active.
 *   uvm_init.c sets vm->repl = NULL at init and does not call
 *   urepl_state_create — allocation happens only when the REPL server starts.
 * === */

#ifndef UREPL_STATE_H
#define UREPL_STATE_H

struct UVM;

typedef struct UReplState {
    void *server;    /* was vm->repl_server; UReplServer * cast to void */
} UReplState;

/* urepl_state_create: allocate a zeroed UReplState wrapper for vm.
 * Returns NULL on OOM.  rs->server is initialised to NULL; the caller
 * (urbi_repl_serve) sets it to the heap-allocated UReplServer pointer.
 * urepl_state_destroy: free rs.  NULL-tolerant.  Does NOT free rs->server;
 * ownership stays with urbi_repl_stop (which calls free(server) separately). */
UReplState *urepl_state_create(struct UVM *vm);
void        urepl_state_destroy(struct UVM *vm, UReplState *rs);

#endif /* UREPL_STATE_H */
