/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_queue.c
 *
 * REPL-02: ring overflow frame-boundary alignment + one-shot error envelope.
 *
 * Two scenarios:
 *
 *   ring_overflow_frame_boundary_aligned
 *     Fill a small UReplRingbuf with newline-terminated messages past
 *     capacity.  After overflow, verify that:
 *       (a) urepl_ringbuf_overflow_consume returns true exactly once,
 *       (b) the first readable byte from the ring starts a complete frame
 *           (i.e. is the first byte of a '\n'-terminated line, not a
 *           mid-frame remnant).
 *
 *   ring_overflow_dispatch_emits_one_error_envelope
 *     Overflow a session's output ring, then drive one eval job through
 *     urepl_dispatch_job.  The first line in the session's output must be
 *     the overflow error envelope and must appear exactly once. */

#include "utest.h"

#ifdef URBI_ENABLE_REPL

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "repl/urepl.h"
#include "repl/urepl_dispatch.h"
#include "repl/urepl_queue.h"
#include "repl/urepl_ndjson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ---- Helpers -------------------------------------------------------------- */

static UReplServer *
mk_server_small_ring(UVM *vm, size_t ring_cap)
{
    UReplConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.bind_addr          = "127.0.0.1";
    cfg.tcp_port           = -1;
    cfg.output_ringbuf_cap = ring_cap;
    return urbi_repl_serve(vm, &cfg, NULL);
}

/* ---- Test 1: ring frame-boundary alignment -------------------------------- */

UTEST(ring_overflow_frame_boundary_aligned)
{
    /* Use a small ring so overflow is easy to trigger. */
    UReplRingbuf rb;
    UASSERT_EQ(urepl_ringbuf_init(&rb, 64), URBI_OK);

    /* Write 12 short newline-terminated frames; each is ~8 bytes so the ring
     * overflows after the first ~8 frames (64 / 8 = 8).  Each frame looks
     * like {"n":NN}\n (10 bytes for n < 10, 11 for n >= 10). */
    for (int i = 0; i < 12; i++) {
        char msg[32];
        int n = snprintf(msg, sizeof(msg), "{\"n\":%d}\n", i);
        urepl_ringbuf_write(&rb, msg, (size_t)n);
    }

    /* Overflow must have been set. */
    UASSERT(urepl_ringbuf_overflow_consume(&rb));

    /* Second consume returns false (flag was cleared). */
    UASSERT(!urepl_ringbuf_overflow_consume(&rb));

    /* Read everything the ring has. */
    char buf[256];
    size_t got = urepl_ringbuf_read(&rb, buf, sizeof(buf) - 1);
    buf[got] = '\0';

    /* (a) There must be at least one complete frame. */
    char *nl = (char *)memchr(buf, '\n', got);
    UASSERT(nl != NULL);

    /* (b) The first byte must be the start of a valid frame, i.e. '{'.
     *     If the drop landed mid-frame the first byte would be some
     *     interior character (digit, '"', ':', etc.) instead. */
    UASSERT_EQ(buf[0], '{');

    /* The first line must end with '}' before the '\n' (well-formed frame). */
    UASSERT(nl > buf);
    UASSERT_EQ(*(nl - 1), '}');

    urepl_ringbuf_destroy(&rb);
}

/* ---- Test 2: dispatch emits exactly one overflow envelope ---------------- */

UTEST(ring_overflow_dispatch_emits_one_error_envelope)
{
    UVM vm;
    UASSERT_EQ(urbi_vm_init(&vm, NULL, NULL), URBI_OK);

    /* Ring cap = 512 bytes — large enough for the overflow error envelope
     * (~82 B) + eval result (~38 B) + done (~25 B) without re-overflowing. */
    UReplServer *server = mk_server_small_ring(&vm, 512);
    UASSERT_NE(server, NULL);

    UReplSession *s = urepl_session_create(server);
    UASSERT_NE(s, NULL);
    s->authed = true;

    /* Fill the session output ring until it overflows.  Each frame is ~44 bytes;
     * 13 frames (572 bytes) exceeds the 512-byte cap, guaranteeing overflow.
     * After the drain the ring is empty, so dispatch's ~145 bytes of output
     * (overflow envelope + result + done) fit without re-overflowing. */
    {
        const char fill[] = "{\"kind\":\"output\",\"msg\":\"xxxxxxxxxxxxxxxxxx\"}\n";
        size_t fill_len = strlen(fill);
        for (int i = 0; i < 13; i++) {
            urepl_ringbuf_write(&s->output, fill, fill_len);
        }
    }
    /* Ring must be overflowed now; consume manually to reset for the dispatch
     * path test — actually DON'T consume: we want dispatch to consume it. */
    UASSERT(urepl_ringbuf_overflow(&s->output));

    /* Drain the ring so it has space for the dispatch output. */
    {
        char sink[512];
        while (urepl_ringbuf_fill(&s->output) > 0)
            urepl_ringbuf_read(&s->output, sink, sizeof(sink));
    }
    /* The overflow flag persists across the drain. */
    UASSERT(urepl_ringbuf_overflow(&s->output));

    /* Dispatch one eval job — dispatch should consume the flag and emit one
     * overflow error envelope before the normal result/done. */
    char *code = (char *)malloc(4);
    UASSERT_NE(code, NULL);
    memcpy(code, "1+1", 3);
    code[3] = '\0';

    UReplJob *job = (UReplJob *)calloc(1, sizeof(*job));
    UASSERT_NE(job, NULL);
    job->session_id   = s->session_id;
    job->req.id       = 42;
    job->req.op       = UREPL_OP_EVAL;
    job->req.code     = code;
    job->req.code_len = 3;

    urepl_dispatch_job(server, job);

    /* Overflow flag must have been consumed by dispatch. */
    UASSERT(!urepl_ringbuf_overflow(&s->output));

    /* Read the session output. */
    char out[512];
    size_t n = urepl_ringbuf_read(&s->output, out, sizeof(out) - 1);
    out[n] = '\0';

    /* (b) Exactly one overflow error envelope must appear.  It comes before
     *     the result/done so it is the very first line. */
    char *first_nl = (char *)memchr(out, '\n', n);
    UASSERT(first_nl != NULL);

    /* Isolate the first line. */
    char first_line[256];
    size_t first_len = (size_t)(first_nl - out);
    if (first_len >= sizeof(first_line)) first_len = sizeof(first_line) - 1;
    memcpy(first_line, out, first_len);
    first_line[first_len] = '\0';

    /* The first line must be the overflow error envelope. */
    UASSERT(strstr(first_line, "\"kind\":\"error\"") != NULL);
    UASSERT(strstr(first_line, "\"code\":\"overflow\"") != NULL);

    /* Count how many overflow envelopes appear in the total output (must be 1). */
    int overflow_count = 0;
    const char *search = out;
    while ((search = strstr(search, "\"code\":\"overflow\"")) != NULL) {
        overflow_count++;
        search++;
    }
    UASSERT_EQ(overflow_count, 1);

    urbi_repl_stop(server);
    urbi_vm_destroy(&vm);
}

#endif /* URBI_ENABLE_REPL */

void
test_repl_queue_suite(void)
{
#ifdef URBI_ENABLE_REPL
    printf("test_repl_queue\n");
    utest_run("ring_overflow_frame_boundary_aligned",
              ring_overflow_frame_boundary_aligned);
    utest_run("ring_overflow_dispatch_emits_one_error_envelope",
              ring_overflow_dispatch_emits_one_error_envelope);
#else
    printf("test_repl_queue (URBI_ENABLE_REPL=0, skipped)\n");
#endif
}
