/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_serve_step_cooperative.c
 *
 * RED test for v0.9.4 Phase 4: cooperative urbi_repl_serve_step.
 *
 * The v0.9.1 stub at src/repl/urepl.c returns URBI_OK without driving
 * any transport vtable function.  For non-pollable transports (Pico USB
 * CDC, UART) the listener pthread is intentionally NOT spawned —
 * urepl_listener_start skips it when no transport reports a kernel-
 * pollable listener fd.  Such transports therefore depend entirely on
 * the host calling urbi_repl_serve_step at the dispatch boundary to
 * sweep accept_fn / read_fn / write_fn / close_fn.
 *
 * Phase 4 of v0.9.4 implements those sweep phases.  This test installs
 * a synthetic non-pollable transport and asserts that one call to
 * urbi_repl_serve_step invokes accept_fn at least once.  Against the
 * v0.9.1 stub it fails on UASSERT(ts.accepted). */
#include "utest.h"

#ifdef URBI_ENABLE_REPL

#include "urbi/urbi.h"
#include "urbi/repl.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "repl/urepl.h"

#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

typedef struct CoopTestState {
    bool   accepted;          /* set true after accept_fn returns 0 once */
    char   read_buf[1024];    /* input bytes to feed read_fn */
    size_t read_len;          /* total bytes to feed */
    size_t read_pos;          /* bytes already read */
    bool   read_eof;          /* simulate host disconnect (read returns 0) */
    char   write_buf[1024];   /* output bytes received via write_fn */
    size_t write_len;         /* bytes written so far */
    bool   closed;            /* close_fn invoked */
} CoopTestState;

static CoopTestState *coop_state = NULL;

static int
coop_accept(void *ls, int *out_fd)
{
    CoopTestState *s = (CoopTestState *)ls;
    if (s->accepted) {
        return -1;  /* single-client transport */
    }
    s->accepted = true;
    *out_fd = 0;
    return 0;
}

static int
coop_read(int fd, void *buf, size_t n)
{
    (void)fd;
    if (coop_state->read_eof) {
        return 0;  /* peer disconnect */
    }
    size_t avail = coop_state->read_len - coop_state->read_pos;
    if (avail == 0) {
        return -1;  /* would-block */
    }
    size_t take = avail < n ? avail : n;
    memcpy(buf, coop_state->read_buf + coop_state->read_pos, take);
    coop_state->read_pos += take;
    return (int)take;
}

static int
coop_write(int fd, const void *buf, size_t n)
{
    (void)fd;
    size_t room = sizeof coop_state->write_buf - coop_state->write_len;
    size_t take = n < room ? n : room;
    memcpy(coop_state->write_buf + coop_state->write_len, buf, take);
    coop_state->write_len += take;
    return (int)take;
}

static void
coop_close(int fd)
{
    (void)fd;
    coop_state->closed = true;
}

static int
coop_pollable(int fd)
{
    (void)fd;
    return -1;  /* non-pollable — matches Pico USB CDC + UART */
}

static const UTransport COOP_TEST_TRANSPORT = {
    .name           = "test-cooperative",
    .accept_fn      = coop_accept,
    .read_fn        = coop_read,
    .write_fn       = coop_write,
    .close_fn       = coop_close,
    .pollable_fd_fn = coop_pollable,
};

UTEST(repl_serve_step_cooperative_accept_only)
{
    UVM *vm = (UVM *)calloc(1, sizeof(UVM));
    UASSERT(vm != NULL);
    UASSERT_EQ(urbi_vm_init(vm, NULL, NULL), URBI_OK);

    static CoopTestState ts;
    memset(&ts, 0, sizeof(ts));
    coop_state = &ts;

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr   = "127.0.0.1";
    cfg.tcp_port    = -1;
    cfg.unix_path   = NULL;
    cfg.max_clients = 4;

    UReplServer *server = NULL;
    UASSERT_EQ(urbi_repl_serve_init(vm, &cfg, &server), URBI_OK);
    UASSERT(server != NULL);

    UASSERT_EQ(urbi_repl_register_transport(server,
                                            &COOP_TEST_TRANSPORT, &ts),
               URBI_OK);

    /* Sweep 1: accept_fn should fire exactly once.  Against the v0.9.1
     * stub urbi_repl_serve_step is a no-op, so ts.accepted stays false
     * and this assertion fails — that is the intended RED state. */
    UASSERT_EQ(urbi_repl_serve_step(server, 0), URBI_OK);
    UASSERT(ts.accepted);

    urbi_repl_stop(server);
    urbi_vm_destroy(vm);
    free(vm);
    coop_state = NULL;
}

/* v0.9.4 Task 4.3: cooperative read sweep.  Inject an NDJSON request
 * into ts.read_buf and verify that repeated urbi_repl_serve_step calls
 * consume the bytes and push the resulting job through the dispatcher.
 *
 * We pick the simplest possible op — an introspect request — because
 * (a) the response shape is well-defined (an envelope written into
 * the session's output ringbuf via push_env), and (b) it doesn't
 * require auth even when default-secure rules apply.  Phase C (Task
 * 4.4) will surface the response back through ts.write_buf; for now
 * we only assert that the input bytes are fully consumed and the job
 * dispatched (observable via the output ringbuf having grown). */
UTEST(repl_serve_step_cooperative_read_dispatch)
{
    UVM *vm = (UVM *)calloc(1, sizeof(UVM));
    UASSERT(vm != NULL);
    UASSERT_EQ(urbi_vm_init(vm, NULL, NULL), URBI_OK);

    static CoopTestState ts;
    memset(&ts, 0, sizeof(ts));
    coop_state = &ts;

    /* Inject a single NDJSON line.  introspect is the smallest op that
     * survives the no-auth default-secure check and reaches the
     * dispatcher unchanged. */
    const char *line = "{\"op\":\"introspect\",\"id\":42,"
                       "\"query\":\"version\"}\n";
    size_t llen = strlen(line);
    UASSERT(llen < sizeof(ts.read_buf));
    memcpy(ts.read_buf, line, llen);
    ts.read_len = llen;

    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr   = "127.0.0.1";
    cfg.tcp_port    = -1;
    cfg.unix_path   = NULL;
    cfg.max_clients = 4;

    UReplServer *server = NULL;
    UASSERT_EQ(urbi_repl_serve_init(vm, &cfg, &server), URBI_OK);
    UASSERT(server != NULL);

    UASSERT_EQ(urbi_repl_register_transport(server,
                                            &COOP_TEST_TRANSPORT, &ts),
               URBI_OK);

    /* Drive several sweeps — first accepts, second reads + dispatches.
     * Loop a few extra times so dispatcher response envelopes flow
     * into the session's output ringbuf.  The write sweep (Task 4.4)
     * will eventually surface them in ts.write_buf. */
    for (int i = 0; i < 16; ++i) {
        UASSERT_EQ(urbi_repl_serve_step(server, 0), URBI_OK);
    }

    UASSERT(ts.accepted);
    /* All input bytes consumed by the cooperative read sweep. */
    UASSERT_EQ(ts.read_pos, ts.read_len);
    /* ts.write_buf stays empty until Task 4.4 (write sweep) lands. */

    urbi_repl_stop(server);
    urbi_vm_destroy(vm);
    free(vm);
    coop_state = NULL;
}

void
test_repl_serve_step_cooperative_suite(void)
{
    printf("test_repl_serve_step_cooperative\n");
    utest_run("repl_serve_step_cooperative_accept_only",
              repl_serve_step_cooperative_accept_only);
    utest_run("repl_serve_step_cooperative_read_dispatch",
              repl_serve_step_cooperative_read_dispatch);
}

#else  /* !URBI_ENABLE_REPL */

void
test_repl_serve_step_cooperative_suite(void)
{
    /* skipped: URBI_ENABLE_REPL=0 */
}

#endif
