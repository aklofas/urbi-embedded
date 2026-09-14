/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/urealm.h — realm lifecycle for the refound/core runtime.
 *
 * A realm is one script world: its own `globals` object, its own strands,
 * and its own output writer.  Every realm's globals inherits from the one
 * per-VM `root_globals` object that holds every built-in, so creating a
 * realm allocates the realm cell and the globals object and nothing else
 * — the whole standard library is shared through the proto chain rather
 * than copied.
 *
 * WHERE THE STRUCT LIVES.  `struct URealm` itself is completed in
 * rt/uexec.h, not here, because the dispatch loop dereferences
 * `s->realm->globals` on the OP_LOAD_REALM_GLOBAL path and uexec sits
 * BELOW urealm in the include order (tests/scripts/check_rt_layering.sh
 * pins that order, and uexec.h may not include this header).  This
 * header owns the realm's behaviour — creation, teardown, the writer —
 * and is what every layer above the exec core includes. */

#ifndef URT_REALM_H
#define URT_REALM_H
#include "rt/uexec.h"

/* Allocates the realm cell and its globals object, links the realm onto
 * vm->realms, and installs the per-realm `Realm` self-reference slot
 * (CONSTANT).  The first realm created becomes vm->main_realm.
 *
 * `globals`'s proto is vm->root_globals, so every built-in — including
 * the Lobby — is visible from every realm.  NULL on OOM, with nothing
 * linked.
 *
 * root_tag stays NULL until the scheduler task creates real tags; realm
 * teardown therefore has no strands to stop yet. */
URealm *urealm_new(UVM *vm);

/* Unlinks the realm from vm->realms.  The realm cell, its globals and
 * everything below them are reclaimed by the next collection once
 * nothing else refers to them.  Stopping root_tag (and with it the
 * realm's strands) lands with the scheduler task.  Freeing the main
 * realm is refused: urbi_realm_main must keep returning a live realm. */
void urealm_free(UVM *vm, URealm *r);

/* GC: marks the realm's globals and every strand threaded onto it. */
void urealm_trace(UVM *vm, URealm *r);

/* Writes one message on one channel.  Prefers the realm's own writer and
 * falls back to the VM-wide one; with neither set the message is
 * dropped.  This is what Lobby.echo and the Channel protos go through. */
void urealm_write(UVM *vm, URealm *r, const char *chan, size_t cl,
                  const char *msg, size_t ml);

#endif
