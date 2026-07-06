/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_transport_posix.h — POSIX errno-to-rc helper.
 *
 * Included by POSIX transport TUs (pty, uart-linux, tcp) to translate a
 * POSIX errno value to the UTransport read_fn / write_fn return convention. */
#ifndef UREPL_TRANSPORT_POSIX_H
#define UREPL_TRANSPORT_POSIX_H

#include <errno.h>

/* Translate a POSIX errno to a UTransport I/O return code.
 * Transient errors (EAGAIN / EWOULDBLOCK / EINTR) return -1 (caller
 * retries).  All other errors return -errno (negative hard error). */
static inline int
urepl_posix_errno_rc(int err)
{
    if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR)
        return -1;
    return -err;
}

#endif /* UREPL_TRANSPORT_POSIX_H */
