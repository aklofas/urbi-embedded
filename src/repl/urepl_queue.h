/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_queue.h - MPSC eval-submission queue + SPSC output ringbuf
 *
 * Two concurrency primitives used by the REPL dispatcher:
 *
 *   UReplQueue — multiple-producer single-consumer linked list of
 *     parsed jobs.  Producers are per-connection reader threads (Phase
 *     3); consumer is the single dispatcher thread (Phase 3 +
 *     Task 13's synchronous helper).  Intrusive linked list; serialized
 *     by a mutex.  A cond var lets the consumer block until non-empty.
 *
 *   UReplRingbuf — single-producer single-consumer byte ring used as
 *     the per-session output buffer.  Producer is the dispatcher
 *     thread driving the realm's eval (one at a time per session);
 *     consumer is the per-connection writer thread (Phase 3).  Lock-
 *     based for v0.9.1 simplicity — lock-free atomic variant is in
 *     the backlog for v1.0-rc.  Overflow policy: drop oldest bytes
 *     until the new write fits (oldest-output-loss matches the spec
 *     §6 expectation that long-running urbiscript tracing may exceed
 *     a slow client's drain rate). */
#ifndef SRC_REPL_UREPL_QUEUE_H
#define SRC_REPL_UREPL_QUEUE_H

#include "repl/urepl_ndjson.h"

#include "urepl_threading.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A parsed job, owned by the queue until drained. */
typedef struct UReplJob {
    UReplNdjsonReq    req;
    uint32_t          session_id;
    struct UReplJob  *next;
} UReplJob;

typedef struct UReplQueue {
    UReplJob       *head;
    UReplJob       *tail;
    size_t          count;
    urbi_mutex_t    mutex;
    urbi_cond_t     cond_nonempty;
    bool            inited;
} UReplQueue;

/* Initialize an MPMC job queue.  Returns 0 on success, non-zero on
 * pthread setup failure (mutex / cond init). */
int  urepl_queue_init    (UReplQueue *q);

/* Tear down a queue.  Frees any jobs left in flight via
 * urepl_ndjson_free_req + free on each node. */
void urepl_queue_destroy (UReplQueue *q);

/* Push takes ownership of *job (the queue calls urepl_ndjson_free_req
 * + free on the node when drained-and-freed by the consumer). */
int  urepl_queue_push        (UReplQueue *q, UReplJob *job);

/* Drain returns the entire queue as a linked list (caller walks ->next
 * and frees each).  Returns NULL if the queue is empty.  Non-blocking. */
UReplJob *urepl_queue_drain_all(UReplQueue *q);

/* Wait until at least one job is available, then drain.  Returns NULL
 * on shutdown (signaled by urepl_queue_signal_shutdown). */
UReplJob *urepl_queue_wait_drain(UReplQueue *q);

/* Signal a waiting consumer to wake up (e.g. on server shutdown). */
void urepl_queue_signal_shutdown(UReplQueue *q);

/* Return the current number of jobs in the queue.  Snapshot — the value
 * may be stale by the time the caller observes it under multi-producer /
 * multi-consumer load. */
size_t urepl_queue_count(UReplQueue *q);

/* ---- SPSC byte ringbuf ------------------------------------------------ */

typedef struct UReplRingbuf {
    char           *buf;
    size_t          cap;       /* total capacity in bytes */
    size_t          read_pos;
    size_t          fill;      /* bytes currently in buffer */
    bool            overflow;  /* sticky: set on any drop */
    urbi_mutex_t    mutex;
    bool            inited;
} UReplRingbuf;

/* Initialize a single-producer/single-consumer byte ringbuf with the
 * given capacity.  Returns 0 on success, non-zero on allocation /
 * pthread_mutex_init failure. */
int    urepl_ringbuf_init   (UReplRingbuf *rb, size_t cap);

/* Tear down a ringbuf.  Frees the backing buffer and destroys the
 * mutex.  Safe to call on a partially-initialized ringbuf. */
void   urepl_ringbuf_destroy(UReplRingbuf *rb);

/* Write n bytes.  Drops oldest bytes if needed.  Returns bytes written
 * (always == n unless n > cap, in which case only the trailing cap
 * bytes are retained and overflow is set). */
size_t urepl_ringbuf_write(UReplRingbuf *rb, const char *data, size_t n);

/* Read up to dst_cap bytes into dst.  Returns bytes read (0 on empty). */
size_t urepl_ringbuf_read (UReplRingbuf *rb, char *dst, size_t dst_cap);

/* Number of bytes currently buffered. */
size_t urepl_ringbuf_fill (UReplRingbuf *rb);

/* Sticky overflow flag; cleared by reset. */
bool   urepl_ringbuf_overflow(UReplRingbuf *rb);

/* Read-and-clear the overflow flag.  Returns true once after an overflow,
 * then false until the next overflow event.  Intended for the dispatch path
 * to emit exactly one overflow error envelope per overflow event. */
bool   urepl_ringbuf_overflow_consume(UReplRingbuf *rb);

#endif /* SRC_REPL_UREPL_QUEUE_H */
