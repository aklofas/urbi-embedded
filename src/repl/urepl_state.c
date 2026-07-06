/* SPDX-License-Identifier: BSD-3-Clause */
/* === v0.10.4: UReplState lifecycle (audit-1 F8). === */

#include "repl/urepl_state.h"
#include "vm/uvm.h"
#include "runtime/umacros.h"  /* urbi_zero */

/* urepl_state_create: allocate a UReplState wrapper.
 *
 * Called by urbi_repl_serve (urepl.c) when a server is started against vm.
 * Returns NULL on OOM.  The rs->server field is initialised to NULL; the
 * caller sets it to the heap-allocated UReplServer pointer. */
UReplState *
urepl_state_create(struct UVM *vm)
{
    if (vm == NULL || vm->alloc_fn == NULL) return NULL;

    UReplState *rs = (UReplState *)vm->alloc_fn(NULL, sizeof(UReplState),
                                                 vm->alloc_ud);
    if (rs == NULL) return NULL;
    urbi_zero(rs, sizeof(UReplState));
    return rs;
}

/* urepl_state_destroy: free the UReplState wrapper.
 *
 * NULL-tolerant.  Does NOT free rs->server — ownership stays with
 * urbi_repl_stop, which calls free(server) after clearing vm->repl. */
void
urepl_state_destroy(struct UVM *vm, UReplState *rs)
{
    if (rs == NULL) return;
    if (vm == NULL || vm->alloc_fn == NULL) return;
    vm->alloc_fn(rs, 0, vm->alloc_ud);
}
