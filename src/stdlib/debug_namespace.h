/* SPDX-License-Identifier: BSD-3-Clause */
/* debug_namespace.h — the Debug prototype and the one introspection
 * primitive behind it.
 *
 * `Debug.coros()` and the REPL's `{"op":"introspect","what":"coros"}`
 * answer with the same bytes, because both call urbi_introspect_coros.
 * The builder lives on the stdlib side rather than in src/repl so the
 * dependency runs repl -> stdlib and a build with no REPL still has the
 * Debug namespace.
 *
 * The other eight introspection primitives the old REPL carried (tags,
 * watchers, events, profile, gc, lobbies, stack, slots) are not here:
 * each walked a runtime structure that no longer exists in that shape,
 * and nothing in the corpus asks for them.  Nothing in this tree
 * promises when, or whether, they come back. */

#ifndef URBI_STDLIB_DEBUG_NAMESPACE_H
#define URBI_STDLIB_DEBUG_NAMESPACE_H

#include "rt/ustdlib_glue.h"

enum { USTDLIB_DEBUG_NMETHODS = 1 };
extern const UMethodDef ustdlib_debug_methods[USTDLIB_DEBUG_NMETHODS];

/* Emits {"coros":[...]} into buf[0..cap).  On success returns URBI_OK and
 * sets *out_n to the byte count written (no trailing NUL is counted, but
 * one is always planted).  A buffer too small for the COMPLETE object is
 * URBI_ERR_INVALID_ARG with *out_n zero and buf emptied — never a
 * silently truncated answer. */
int urbi_introspect_coros(UVM *vm, char *buf, size_t cap, size_t *out_n);

#endif
