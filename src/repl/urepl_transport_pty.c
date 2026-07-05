/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_transport_pty.c - Linux pty-pair test harness
 *
 * Spec §10.1.  openpty() allocates a (master, slave) fd pair under
 * /dev/ptmx; the slave fd is what the REPL listener thread treats as
 * the "client" — it's already connected.  The accept_fn returns the
 * pre-opened slave fd exactly once, then -1 (would-block) on every
 * subsequent call, matching the spec's single-client UART contract.
 *
 * The slave fd is set non-blocking so the reader thread's read_fn
 * returns -1 (EAGAIN) cleanly on empty drain rather than blocking and
 * holding up shutdown.  The master fd is also non-blocking so the
 * driving test can poll without blocking the test thread.
 *
 * Listener integration: urepl_listener.c's listener_pollable_fd
 * type-discriminates on &UREPL_PTY_TRANSPORT and lifts the slave fd
 * out of UPtyState — this makes the listener's poll() wake on master-
 * side writes promptly.  After the one-shot accept returns the slave
 * fd to the reader thread, subsequent accept_fn calls return -1 and
 * the listener's poll just spins on the stop_eventfd (the slave fd is
 * the same fd the reader thread is now reading from, but the kernel
 * doesn't mind two pollers).
 *
 * Only compiled when URBI_ENABLE_REPL=1. */
#include "repl/urepl_transport_pty.h"
#include "repl/urepl_transport_common.h"
#include "repl/urepl_transport_posix.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

struct UPtyState {
    int  master_fd;
    int  slave_fd;
    bool accepted;
};

UPtyState *
urepl_pty_state_create(void)
{
    UPtyState *st = (UPtyState *)calloc(1, sizeof(*st));
    if (st == NULL) {
        return NULL;
    }
    if (openpty(&st->master_fd, &st->slave_fd, NULL, NULL, NULL) < 0) {
        free(st);
        return NULL;
    }
    int sl = fcntl(st->slave_fd, F_GETFL, 0);
    if (sl >= 0) {
        (void)fcntl(st->slave_fd, F_SETFL, sl | O_NONBLOCK);
    }
    int ml = fcntl(st->master_fd, F_GETFL, 0);
    if (ml >= 0) {
        (void)fcntl(st->master_fd, F_SETFL, ml | O_NONBLOCK);
    }
    return st;
}

void
urepl_pty_state_destroy(UPtyState *st)
{
    if (st == NULL) {
        return;
    }
    if (st->master_fd >= 0) {
        (void)close(st->master_fd);
        st->master_fd = -1;
    }
    if (st->slave_fd >= 0) {
        (void)close(st->slave_fd);
        st->slave_fd = -1;
    }
    free(st);
}

int
urepl_pty_master_fd(const UPtyState *st)
{
    if (st == NULL) {
        return -1;
    }
    return st->master_fd;
}

int
urepl_pty_slave_fd(const UPtyState *st)
{
    if (st == NULL) {
        return -1;
    }
    return st->slave_fd;
}

static int
pty_accept(void *listener_state, int *out_client_fd)
{
    UPtyState *st = (UPtyState *)listener_state;
    UREPL_ACCEPT_ONCE(st, out_client_fd, st->slave_fd);
}

static int
pty_read(int client_fd, void *buf, size_t n)
{
    ssize_t r = read(client_fd, buf, n);
    if (r < 0) {
        /* When the master side is closed, the slave read can return
         * EIO on Linux (rather than 0 / EOF).  Treat as clean EOF so
         * the reader thread tears down without spinning. */
        if (errno == EIO)
            return 0;
        return urepl_posix_errno_rc(errno);
    }
    return (int)r;  /* 0 = EOF */
}

static int
pty_write(int client_fd, const void *buf, size_t n)
{
    ssize_t w = write(client_fd, buf, n);
    if (w < 0) {
        if (errno == EIO)
            return 0;
        return urepl_posix_errno_rc(errno);
    }
    return (int)w;
}

static void
pty_close(int client_fd)
{
    (void)client_fd;
    /* No-op: the UPtyState owns master + slave fds.  Closing here
     * would leave a dangling slave_fd in the state struct and break
     * urepl_pty_state_destroy. */
}

static int
pty_pollable_fd(int client_fd)
{
    return client_fd;
}

const UTransport UREPL_PTY_TRANSPORT = {
    .name           = "pty",
    .accept_fn      = pty_accept,
    .read_fn        = pty_read,
    .write_fn       = pty_write,
    .close_fn       = pty_close,
    .pollable_fd_fn = pty_pollable_fd
};
