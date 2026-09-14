/* SPDX-License-Identifier: BSD-3-Clause */
/* lobby_native.h — the Lobby prototype's native methods and the
 * session-registry hook.
 *
 * The Lobby sits in every realm's prototype chain (globals ->
 * root_globals -> Lobby -> Object), so `echo("hi")` resolves unqualified
 * from the CLI, from a batch file and from a REPL line alike.  Three
 * primitives are native because they reach the writer:
 *
 *   __builtin_lobby_send(msg, tag, prefix)      — frame + write on the
 *       CURRENT realm's writer.  Everything a program echoes goes here.
 *   __builtin_lobby_send_to(lobby, msg, tag, prefix) — the same frame
 *       written on the writer of the realm whose globals is `lobby`.
 *       This is what makes `wall` a broadcast rather than three copies
 *       of the sender's own output.
 *   echo(msg[, tag[, prefix]])                  — the defaulted wrapper,
 *       native so the defaults do not depend on default-parameter
 *       lowering and so a non-String argument still prints.
 *
 * `Lobby.lobbies` is a List of every live realm's globals object.  It is
 * maintained in rt/urealm.c (urealm_new pushes, urealm_free removes),
 * not here: a realm comes and goes below the stdlib layer.  This file
 * only CREATES the slot, at boot, through urbi_lobby_init. */

#ifndef URBI_STDLIB_LOBBY_NATIVE_H
#define URBI_STDLIB_LOBBY_NATIVE_H

#include "rt/ustdlib_glue.h"

enum { USTDLIB_LOBBY_NMETHODS = 3 };
extern const UMethodDef ustdlib_lobby_methods[USTDLIB_LOBBY_NMETHODS];

/* Installs the `lobbies` List on the Lobby prototype.  Called from
 * uboot_init's slot pass, before the read-only seal and before the
 * script overlay, so the overlay's `wall` can already read it. */
int urbi_lobby_init(UVM *vm);

#endif
