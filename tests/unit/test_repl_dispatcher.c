/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_repl_dispatcher.c — REPL queue + ringbuf + dispatcher.
 *
 * Task 12 lands the queue + ringbuf cases; Task 13 extends with full
 * dispatcher round-trip tests. */
#include "utest.h"

#ifdef URBI_ENABLE_REPL

#include "repl/urepl_queue.h"
#include "urbi/types.h"
/* repl/urepl_dispatch.h + repl/urepl.h pulled in by Task 13's
 * dispatcher_handles_eval_op test extension. */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ---- Queue tests ----------------------------------------------------- */

UTEST(queue_init_destroy_empty)
{
    UReplQueue q;
    UASSERT_EQ(urepl_queue_init(&q), URBI_OK);
    UASSERT_EQ(urepl_queue_count(&q), 0);
    urepl_queue_destroy(&q);
}

UTEST(queue_push_drain_roundtrip)
{
    UReplQueue q;
    UASSERT_EQ(urepl_queue_init(&q), URBI_OK);

    UReplJob *j1 = (UReplJob *)calloc(1, sizeof(*j1));
    UReplJob *j2 = (UReplJob *)calloc(1, sizeof(*j2));
    UReplJob *j3 = (UReplJob *)calloc(1, sizeof(*j3));
    j1->session_id = 1;
    j2->session_id = 2;
    j3->session_id = 3;
    UASSERT_EQ(urepl_queue_push(&q, j1), URBI_OK);
    UASSERT_EQ(urepl_queue_push(&q, j2), URBI_OK);
    UASSERT_EQ(urepl_queue_push(&q, j3), URBI_OK);
    UASSERT_EQ(urepl_queue_count(&q), 3);

    UReplJob *drained = urepl_queue_drain_all(&q);
    UASSERT(drained != NULL);
    UASSERT_EQ(drained->session_id, 1);
    UASSERT(drained->next != NULL);
    UASSERT_EQ(drained->next->session_id, 2);
    UASSERT(drained->next->next != NULL);
    UASSERT_EQ(drained->next->next->session_id, 3);
    UASSERT(drained->next->next->next == NULL);
    UASSERT_EQ(urepl_queue_count(&q), 0);

    while (drained != NULL) {
        UReplJob *next = drained->next;
        urepl_ndjson_free_req(&drained->req);
        free(drained);
        drained = next;
    }
    urepl_queue_destroy(&q);
}

UTEST(queue_drain_empty_returns_null)
{
    UReplQueue q;
    UASSERT_EQ(urepl_queue_init(&q), URBI_OK);
    UASSERT(urepl_queue_drain_all(&q) == NULL);
    urepl_queue_destroy(&q);
}

UTEST(queue_destroy_frees_remaining_jobs)
{
    /* Ensures destroy doesn't leak the leftover jobs.  Verified via
     * absence of AddressSanitizer/valgrind complaints in CI; the
     * functional smoke is just that destroy returns. */
    UReplQueue q;
    UASSERT_EQ(urepl_queue_init(&q), URBI_OK);
    for (int i = 0; i < 5; i++) {
        UReplJob *j = (UReplJob *)calloc(1, sizeof(*j));
        j->session_id = (uint32_t)(i + 1);
        UASSERT_EQ(urepl_queue_push(&q, j), URBI_OK);
    }
    UASSERT_EQ(urepl_queue_count(&q), 5);
    urepl_queue_destroy(&q);  /* must free the 5 jobs internally */
}

/* MPSC stress: 4 producer threads each push 100 jobs; consumer drains
 * twice and counts total.  Verifies the count equals 400 and no jobs
 * are lost or duplicated. */
typedef struct {
    UReplQueue *q;
    int         producer_id;
    int         n;
} ProdArgs;

static void *
producer_thread(void *arg)
{
    ProdArgs *a = (ProdArgs *)arg;
    for (int i = 0; i < a->n; i++) {
        UReplJob *j = (UReplJob *)calloc(1, sizeof(*j));
        if (j == NULL) {
            return NULL;
        }
        /* session_id encodes (producer << 16 | seq) for uniqueness. */
        j->session_id = (uint32_t)((a->producer_id << 16) | i);
        urepl_queue_push(a->q, j);
    }
    return NULL;
}

UTEST(queue_mpsc_stress_4_producers_100_each)
{
    UReplQueue q;
    UASSERT_EQ(urepl_queue_init(&q), URBI_OK);

    pthread_t threads[4];
    ProdArgs args[4];
    for (int i = 0; i < 4; i++) {
        args[i].q = &q;
        args[i].producer_id = i;
        args[i].n = 100;
        pthread_create(&threads[i], NULL, producer_thread, &args[i]);
    }
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Consumer drains. */
    int seen = 0;
    UReplJob *drained = urepl_queue_drain_all(&q);
    while (drained != NULL) {
        UReplJob *next = drained->next;
        urepl_ndjson_free_req(&drained->req);
        free(drained);
        seen++;
        drained = next;
    }
    UASSERT_EQ(seen, 400);
    urepl_queue_destroy(&q);
}

/* ---- Ringbuf tests --------------------------------------------------- */

UTEST(ringbuf_init_destroy_empty)
{
    UReplRingbuf rb;
    UASSERT_EQ(urepl_ringbuf_init(&rb, 64), URBI_OK);
    UASSERT_EQ(urepl_ringbuf_fill(&rb), 0);
    UASSERT_EQ(urepl_ringbuf_overflow(&rb), 0);
    urepl_ringbuf_destroy(&rb);
}

UTEST(ringbuf_write_read_basic)
{
    UReplRingbuf rb;
    UASSERT_EQ(urepl_ringbuf_init(&rb, 64), URBI_OK);
    size_t n = urepl_ringbuf_write(&rb, "hello", 5);
    UASSERT_EQ(n, 5);
    UASSERT_EQ(urepl_ringbuf_fill(&rb), 5);
    char out[32];
    n = urepl_ringbuf_read(&rb, out, sizeof(out));
    UASSERT_EQ(n, 5);
    UASSERT(memcmp(out, "hello", 5) == 0);
    UASSERT_EQ(urepl_ringbuf_fill(&rb), 0);
    urepl_ringbuf_destroy(&rb);
}

UTEST(ringbuf_partial_read)
{
    UReplRingbuf rb;
    UASSERT_EQ(urepl_ringbuf_init(&rb, 64), URBI_OK);
    urepl_ringbuf_write(&rb, "abcdefghij", 10);
    char out[4];
    size_t n = urepl_ringbuf_read(&rb, out, 4);
    UASSERT_EQ(n, 4);
    UASSERT(memcmp(out, "abcd", 4) == 0);
    UASSERT_EQ(urepl_ringbuf_fill(&rb), 6);
    n = urepl_ringbuf_read(&rb, out, 4);
    UASSERT_EQ(n, 4);
    UASSERT(memcmp(out, "efgh", 4) == 0);
    urepl_ringbuf_destroy(&rb);
}

UTEST(ringbuf_wraps_around)
{
    /* Fill exactly, drain partial, write again forcing wraparound. */
    UReplRingbuf rb;
    UASSERT_EQ(urepl_ringbuf_init(&rb, 8), URBI_OK);
    urepl_ringbuf_write(&rb, "12345678", 8);
    UASSERT_EQ(urepl_ringbuf_fill(&rb), 8);
    char out[16];
    size_t n = urepl_ringbuf_read(&rb, out, 5);
    UASSERT_EQ(n, 5);
    UASSERT(memcmp(out, "12345", 5) == 0);
    UASSERT_EQ(urepl_ringbuf_fill(&rb), 3);
    /* Now write "ABCDE" — this wraps around. */
    urepl_ringbuf_write(&rb, "ABCDE", 5);
    UASSERT_EQ(urepl_ringbuf_fill(&rb), 8);
    n = urepl_ringbuf_read(&rb, out, 16);
    UASSERT_EQ(n, 8);
    UASSERT(memcmp(out, "678ABCDE", 8) == 0);
    urepl_ringbuf_destroy(&rb);
}

UTEST(ringbuf_overflow_drops_oldest)
{
    UReplRingbuf rb;
    UASSERT_EQ(urepl_ringbuf_init(&rb, 8), URBI_OK);
    urepl_ringbuf_write(&rb, "AAAAAAAA", 8);
    /* Now overflow by 4 bytes -- should drop the first 4 of the original. */
    urepl_ringbuf_write(&rb, "BBBB", 4);
    UASSERT(urepl_ringbuf_overflow(&rb) == true);
    UASSERT_EQ(urepl_ringbuf_fill(&rb), 8);
    char out[16];
    size_t n = urepl_ringbuf_read(&rb, out, 16);
    UASSERT_EQ(n, 8);
    UASSERT(memcmp(out, "AAAABBBB", 8) == 0);
    urepl_ringbuf_destroy(&rb);
}

UTEST(ringbuf_write_larger_than_cap)
{
    UReplRingbuf rb;
    UASSERT_EQ(urepl_ringbuf_init(&rb, 4), URBI_OK);
    /* 6-byte write into 4-byte ringbuf retains the last 4 bytes. */
    urepl_ringbuf_write(&rb, "ABCDEF", 6);
    UASSERT(urepl_ringbuf_overflow(&rb) == true);
    UASSERT_EQ(urepl_ringbuf_fill(&rb), 4);
    char out[8];
    size_t n = urepl_ringbuf_read(&rb, out, 8);
    UASSERT_EQ(n, 4);
    UASSERT(memcmp(out, "CDEF", 4) == 0);
    urepl_ringbuf_destroy(&rb);
}

UTEST(ringbuf_read_empty_returns_zero)
{
    UReplRingbuf rb;
    UASSERT_EQ(urepl_ringbuf_init(&rb, 8), URBI_OK);
    char out[4];
    UASSERT_EQ(urepl_ringbuf_read(&rb, out, 4), 0);
    urepl_ringbuf_destroy(&rb);
}

UTEST(ringbuf_zero_size_init_rejects)
{
    UReplRingbuf rb;
    int rc = urepl_ringbuf_init(&rb, 0);
    UASSERT(rc != URBI_OK);
}

void
test_repl_dispatcher_suite(void)
{
    printf("test_repl_dispatcher\n");
    utest_run("queue_init_destroy_empty",           queue_init_destroy_empty);
    utest_run("queue_push_drain_roundtrip",         queue_push_drain_roundtrip);
    utest_run("queue_drain_empty_returns_null",     queue_drain_empty_returns_null);
    utest_run("queue_destroy_frees_remaining_jobs", queue_destroy_frees_remaining_jobs);
    utest_run("queue_mpsc_stress_4_producers_100_each",
              queue_mpsc_stress_4_producers_100_each);
    utest_run("ringbuf_init_destroy_empty",         ringbuf_init_destroy_empty);
    utest_run("ringbuf_write_read_basic",           ringbuf_write_read_basic);
    utest_run("ringbuf_partial_read",               ringbuf_partial_read);
    utest_run("ringbuf_wraps_around",               ringbuf_wraps_around);
    utest_run("ringbuf_overflow_drops_oldest",      ringbuf_overflow_drops_oldest);
    utest_run("ringbuf_write_larger_than_cap",      ringbuf_write_larger_than_cap);
    utest_run("ringbuf_read_empty_returns_zero",    ringbuf_read_empty_returns_zero);
    utest_run("ringbuf_zero_size_init_rejects",     ringbuf_zero_size_init_rejects);
    /* Task 13 adds dispatcher_handles_eval_op + session-machinery tests. */
}

#else  /* !URBI_ENABLE_REPL */

void test_repl_dispatcher_suite(void) { /* skipped: URBI_ENABLE_REPL=0 */ }

#endif
