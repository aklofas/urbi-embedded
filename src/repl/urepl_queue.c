/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_queue.c - MPSC job queue + SPSC byte ringbuf
 *
 * Simple mutex-protected implementations.  The byte ringbuf uses an
 * oldest-drop policy on overflow (spec §6: streaming output may exceed
 * a slow client's drain rate; preserve newest output, mark sticky
 * overflow). */
#include "repl/urepl_queue.h"
#include "urbi/types.h"

#include <stdlib.h>
#include <string.h>

/* ---- Job queue ------------------------------------------------------- */

int
urepl_queue_init(UReplQueue *q)
{
    if (q == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    memset(q, 0, sizeof(*q));
    if (UREPL_MUTEX_INIT(&q->mutex) != 0) {
        return URBI_ERR_OOM;
    }
    if (UREPL_COND_INIT(&q->cond_nonempty) != 0) {
        UREPL_MUTEX_DESTROY(&q->mutex);
        return URBI_ERR_OOM;
    }
    q->inited = true;
    return URBI_OK;
}

void
urepl_queue_destroy(UReplQueue *q)
{
    if (q == NULL || !q->inited) {
        return;
    }
    /* Free any remaining jobs. */
    UReplJob *j = q->head;
    while (j != NULL) {
        UReplJob *next = j->next;
        urepl_ndjson_free_req(&j->req);
        free(j);
        j = next;
    }
    UREPL_COND_DESTROY(&q->cond_nonempty);
    UREPL_MUTEX_DESTROY(&q->mutex);
    memset(q, 0, sizeof(*q));
}

int
urepl_queue_push(UReplQueue *q, UReplJob *job)
{
    if (q == NULL || job == NULL || !q->inited) {
        return URBI_ERR_INVALID_ARG;
    }
    job->next = NULL;
    UREPL_MUTEX_LOCK(&q->mutex);
    if (q->tail == NULL) {
        q->head = job;
        q->tail = job;
    } else {
        q->tail->next = job;
        q->tail = job;
    }
    q->count++;
    UREPL_COND_SIGNAL(&q->cond_nonempty);
    UREPL_MUTEX_UNLOCK(&q->mutex);
    return URBI_OK;
}

UReplJob *
urepl_queue_drain_all(UReplQueue *q)
{
    if (q == NULL || !q->inited) {
        return NULL;
    }
    UREPL_MUTEX_LOCK(&q->mutex);
    UReplJob *head = q->head;
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
    UREPL_MUTEX_UNLOCK(&q->mutex);
    return head;
}

UReplJob *
urepl_queue_wait_drain(UReplQueue *q)
{
    if (q == NULL || !q->inited) {
        return NULL;
    }
    UREPL_MUTEX_LOCK(&q->mutex);
    while (q->head == NULL) {
        UREPL_COND_WAIT(&q->cond_nonempty, &q->mutex);
        /* On shutdown signal, head stays NULL and we exit the loop via
         * the broadcast (caller checks for sentinel head == NULL). */
        if (!q->inited) {
            UREPL_MUTEX_UNLOCK(&q->mutex);
            return NULL;
        }
    }
    UReplJob *head = q->head;
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
    UREPL_MUTEX_UNLOCK(&q->mutex);
    return head;
}

void
urepl_queue_signal_shutdown(UReplQueue *q)
{
    if (q == NULL || !q->inited) {
        return;
    }
    UREPL_MUTEX_LOCK(&q->mutex);
    /* No new sentinel — consumer re-checks q->inited after waking. */
    UREPL_COND_BROADCAST(&q->cond_nonempty);
    UREPL_MUTEX_UNLOCK(&q->mutex);
}

size_t
urepl_queue_count(UReplQueue *q)
{
    if (q == NULL || !q->inited) {
        return 0;
    }
    UREPL_MUTEX_LOCK(&q->mutex);
    size_t c = q->count;
    UREPL_MUTEX_UNLOCK(&q->mutex);
    return c;
}

/* ---- SPSC byte ringbuf ----------------------------------------------- */

int
urepl_ringbuf_init(UReplRingbuf *rb, size_t cap)
{
    if (rb == NULL || cap == 0U) {
        return URBI_ERR_INVALID_ARG;
    }
    memset(rb, 0, sizeof(*rb));
    rb->buf = (char *)malloc(cap);
    if (rb->buf == NULL) {
        return URBI_ERR_OOM;
    }
    rb->cap = cap;
    if (UREPL_MUTEX_INIT(&rb->mutex) != 0) {
        free(rb->buf);
        rb->buf = NULL;
        return URBI_ERR_OOM;
    }
    rb->inited = true;
    return URBI_OK;
}

void
urepl_ringbuf_destroy(UReplRingbuf *rb)
{
    if (rb == NULL || !rb->inited) {
        return;
    }
    UREPL_MUTEX_DESTROY(&rb->mutex);
    free(rb->buf);
    memset(rb, 0, sizeof(*rb));
}

/* Internal: write n bytes assuming caller holds lock and n <= cap.  Drops
 * oldest bytes if (fill + n) > cap. */
static void
ringbuf_write_locked(UReplRingbuf *rb, const char *data, size_t n)
{
    if (n > rb->cap) {
        /* Trim to last cap bytes. */
        data += (n - rb->cap);
        n = rb->cap;
    }
    size_t free_space = rb->cap - rb->fill;
    if (n > free_space) {
        size_t drop = n - free_space;
        rb->read_pos = (rb->read_pos + drop) % rb->cap;
        rb->fill -= drop;
        rb->overflow = true;
        /* Extend the drop through the next '\n' so read_pos always starts a
         * complete NDJSON frame.  Without this, the raw-byte drop may land
         * mid-frame, desyncing the client's line parser.  We consume bytes
         * one at a time until we find '\n' (which we also consume, so
         * read_pos lands on the first byte of the following frame) or we
         * exhaust all remaining data (fill → 0, new data written fresh). */
        while (rb->fill > 0) {
            char c = rb->buf[rb->read_pos];
            rb->read_pos = (rb->read_pos + 1) % rb->cap;
            rb->fill -= 1;
            if (c == '\n') {
                break;
            }
        }
    }
    /* Write at (read_pos + fill) wrap. */
    size_t write_pos = (rb->read_pos + rb->fill) % rb->cap;
    size_t first = rb->cap - write_pos;
    if (first > n) first = n;
    memcpy(rb->buf + write_pos, data, first);
    if (n > first) {
        memcpy(rb->buf, data + first, n - first);
    }
    rb->fill += n;
}

size_t
urepl_ringbuf_write(UReplRingbuf *rb, const char *data, size_t n)
{
    if (rb == NULL || !rb->inited || data == NULL || n == 0U) {
        return 0;
    }
    UREPL_MUTEX_LOCK(&rb->mutex);
    size_t written = n;
    if (written > rb->cap) {
        rb->overflow = true;
    }
    ringbuf_write_locked(rb, data, n);
    UREPL_MUTEX_UNLOCK(&rb->mutex);
    return written;
}

size_t
urepl_ringbuf_read(UReplRingbuf *rb, char *dst, size_t dst_cap)
{
    if (rb == NULL || !rb->inited || dst == NULL || dst_cap == 0U) {
        return 0;
    }
    UREPL_MUTEX_LOCK(&rb->mutex);
    size_t avail = rb->fill;
    size_t take = (avail < dst_cap) ? avail : dst_cap;
    if (take > 0U) {
        size_t first = rb->cap - rb->read_pos;
        if (first > take) first = take;
        memcpy(dst, rb->buf + rb->read_pos, first);
        if (take > first) {
            memcpy(dst + first, rb->buf, take - first);
        }
        rb->read_pos = (rb->read_pos + take) % rb->cap;
        rb->fill -= take;
    }
    UREPL_MUTEX_UNLOCK(&rb->mutex);
    return take;
}

size_t
urepl_ringbuf_fill(UReplRingbuf *rb)
{
    if (rb == NULL || !rb->inited) {
        return 0;
    }
    UREPL_MUTEX_LOCK(&rb->mutex);
    size_t f = rb->fill;
    UREPL_MUTEX_UNLOCK(&rb->mutex);
    return f;
}

bool
urepl_ringbuf_overflow(UReplRingbuf *rb)
{
    if (rb == NULL || !rb->inited) {
        return false;
    }
    UREPL_MUTEX_LOCK(&rb->mutex);
    bool o = rb->overflow;
    UREPL_MUTEX_UNLOCK(&rb->mutex);
    return o;
}

/* Read-and-clear the overflow flag.  Returns true once after an overflow,
 * then false until the next overflow event.  Lock discipline: acquires
 * rb->mutex alone (same lock as ringbuf_write_locked); no new lock-order
 * edge is introduced. */
bool
urepl_ringbuf_overflow_consume(UReplRingbuf *rb)
{
    if (rb == NULL || !rb->inited) {
        return false;
    }
    UREPL_MUTEX_LOCK(&rb->mutex);
    bool o = rb->overflow;
    rb->overflow = false;
    UREPL_MUTEX_UNLOCK(&rb->mutex);
    return o;
}

bool
urepl_ringbuf_headroom(UReplRingbuf *rb, size_t n)
{
    if (rb == NULL || !rb->inited) {
        return false;
    }
    UREPL_MUTEX_LOCK(&rb->mutex);
    bool ok = (rb->cap - rb->fill) >= n;
    UREPL_MUTEX_UNLOCK(&rb->mutex);
    return ok;
}
