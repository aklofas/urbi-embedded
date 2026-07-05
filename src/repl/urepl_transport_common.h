/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_transport_common.h — single-client accept-once helper.
 *
 * Included by transport TUs that implement the "return the configured fd
 * exactly once, then -1 forever" accept pattern.  The macro expands a
 * return statement into the enclosing function, so it must appear as the
 * final statement in each accept_fn. */
#ifndef SRC_REPL_UREPL_TRANSPORT_COMMON_H
#define SRC_REPL_UREPL_TRANSPORT_COMMON_H

#include "urbi/types.h"   /* URBI_ERR_INVALID_ARG */
#include <stdbool.h>

/* Single-client accept state machine.  ST is the typed state pointer (already
 * cast from void *); OUT is the int *out_client_fd parameter; CLIENT_VAL is
 * the fd or handle to expose on first accept.
 *
 * Returns URBI_ERR_INVALID_ARG when ST or OUT is NULL, -1 on subsequent calls
 * (no further clients), 0 on first accept with *OUT set.
 *
 * The macro uses 'return' so each transport TU's accept_fn returns directly
 * without extra branching.  ST is evaluated once. */
#define UREPL_ACCEPT_ONCE(st, out, client_val) \
    do {                                         \
        if ((st) == NULL || (out) == NULL)       \
            return URBI_ERR_INVALID_ARG;         \
        if ((st)->accepted)                      \
            return -1;                           \
        (st)->accepted = true;                   \
        *(out) = (int)(client_val);              \
        return 0;                                \
    } while (0)

#endif /* SRC_REPL_UREPL_TRANSPORT_COMMON_H */
