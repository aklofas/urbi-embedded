/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_buffer_transport.c - in-process loopback transport
 *
 * Backed by two SPSC ringbufs:
 *   c2s  — client writes here (urepl_buffer_client_write), server
 *          read_fn drains it.
 *   s2c  — server write_fn writes here, client drains it via
 *          urepl_buffer_client_read.
 *
 * The "fd" returned by accept_fn is purely symbolic (a fixed value of
 * 0); the transport's read/write/close fns route via the state object
 * directly.  In Phase 3 the listener thread will need a way to find the
 * state from the fd; for v0.9.1 there is exactly one buffer transport
 * per server (single-test pattern), so we attach the state on accept
 * via the global pointer the listener thread already holds (the
 * listener_state arg). */
#include "repl/urepl_buffer_transport.h"
#include "repl/urepl_queue.h"
#include "urbi/types.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define BT_DEFAULT_CAP (64u * 1024u)

struct UBufferTransportState {
    UReplRingbuf c2s;    /* client → server */
    UReplRingbuf s2c;    /* server → client */
    bool         accept_consumed;
    pthread_mutex_t accept_mutex;
    /* Single global pointer used by the static vtable functions to map
     * a fd back to the state.  Phase 3's TCP transport will use a real
     * fd-indexed table; for now v0.9.1's single-test pattern means each
     * server gets one buffer transport state, set via accept_fn. */
    /* (See urbi_repl_register_transport's listener_state arg.) */
};

/* Per-fd state lookup.  The buffer transport's accept_fn stores the
 * state in a thread-local for the duration of the test; tests that
 * spin up multiple buffer transports per process will need to lock-
 * step around this, which v0.9.1 doesn't exercise. */
static __thread UBufferTransportState *g_buffer_state = NULL;

UBufferTransportState *
urepl_buffer_transport_create(void)
{
    UBufferTransportState *st = (UBufferTransportState *)calloc(1, sizeof(*st));
    if (st == NULL) {
        return NULL;
    }
    if (urepl_ringbuf_init(&st->c2s, BT_DEFAULT_CAP) != URBI_OK) {
        free(st);
        return NULL;
    }
    if (urepl_ringbuf_init(&st->s2c, BT_DEFAULT_CAP) != URBI_OK) {
        urepl_ringbuf_destroy(&st->c2s);
        free(st);
        return NULL;
    }
    if (pthread_mutex_init(&st->accept_mutex, NULL) != 0) {
        urepl_ringbuf_destroy(&st->c2s);
        urepl_ringbuf_destroy(&st->s2c);
        free(st);
        return NULL;
    }
    return st;
}

void
urepl_buffer_transport_destroy(UBufferTransportState *st)
{
    if (st == NULL) {
        return;
    }
    urepl_ringbuf_destroy(&st->c2s);
    urepl_ringbuf_destroy(&st->s2c);
    pthread_mutex_destroy(&st->accept_mutex);
    free(st);
}

void
urepl_buffer_transport_reset_accept(UBufferTransportState *st)
{
    if (st == NULL) {
        return;
    }
    pthread_mutex_lock(&st->accept_mutex);
    st->accept_consumed = false;
    pthread_mutex_unlock(&st->accept_mutex);
}

/* ---- Vtable impl ----------------------------------------------------- */

static int
bt_accept(void *listener_state, int *out_client_fd)
{
    UBufferTransportState *st = (UBufferTransportState *)listener_state;
    if (st == NULL || out_client_fd == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    int taken = 0;
    pthread_mutex_lock(&st->accept_mutex);
    if (!st->accept_consumed) {
        st->accept_consumed = true;
        taken = 1;
    }
    pthread_mutex_unlock(&st->accept_mutex);
    if (!taken) {
        return -1;  /* signal "would block / no client" */
    }
    /* Map the fd to our state pointer for the duration of the test. */
    g_buffer_state = st;
    *out_client_fd = 0;  /* sentinel fd */
    return 0;
}

static int
bt_read(int client_fd, void *buf, size_t n)
{
    (void)client_fd;
    UBufferTransportState *st = g_buffer_state;
    if (st == NULL || buf == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    return (int)urepl_ringbuf_read(&st->c2s, (char *)buf, n);
}

static int
bt_write(int client_fd, const void *buf, size_t n)
{
    (void)client_fd;
    UBufferTransportState *st = g_buffer_state;
    if (st == NULL || buf == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    return (int)urepl_ringbuf_write(&st->s2c, (const char *)buf, n);
}

static void
bt_close(int client_fd)
{
    (void)client_fd;
    /* Nothing to do — the state outlives the fd by design (tests own
     * the state via _create / _destroy). */
}

static int
bt_pollable_fd(int client_fd)
{
    (void)client_fd;
    return -1;  /* not pollable; tests drive manually */
}

const UTransport UREPL_BUFFER_TRANSPORT = {
    .name           = "buffer",
    .accept_fn      = bt_accept,
    .read_fn        = bt_read,
    .write_fn       = bt_write,
    .close_fn       = bt_close,
    .pollable_fd_fn = bt_pollable_fd
};

/* ---- Client-side helpers --------------------------------------------- */

size_t
urepl_buffer_client_write(UBufferTransportState *st,
                          const void *bytes, size_t n)
{
    if (st == NULL || bytes == NULL || n == 0U) {
        return 0;
    }
    return urepl_ringbuf_write(&st->c2s, (const char *)bytes, n);
}

size_t
urepl_buffer_client_read(UBufferTransportState *st, void *buf, size_t cap)
{
    if (st == NULL || buf == NULL || cap == 0U) {
        return 0;
    }
    return urepl_ringbuf_read(&st->s2c, (char *)buf, cap);
}
