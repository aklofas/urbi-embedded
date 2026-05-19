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
    if (pthread_mutex_init(&q->mutex, NULL) != 0) {
        return URBI_ERR_OOM;
    }
    if (pthread_cond_init(&q->cond_nonempty, NULL) != 0) {
        pthread_mutex_destroy(&q->mutex);
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
    pthread_cond_destroy(&q->cond_nonempty);
    pthread_mutex_destroy(&q->mutex);
    memset(q, 0, sizeof(*q));
}

int
urepl_queue_push(UReplQueue *q, UReplJob *job)
{
    if (q == NULL || job == NULL || !q->inited) {
        return URBI_ERR_INVALID_ARG;
    }
    job->next = NULL;
    pthread_mutex_lock(&q->mutex);
    if (q->tail == NULL) {
        q->head = job;
        q->tail = job;
    } else {
        q->tail->next = job;
        q->tail = job;
    }
    q->count++;
    pthread_cond_signal(&q->cond_nonempty);
    pthread_mutex_unlock(&q->mutex);
    return URBI_OK;
}

UReplJob *
urepl_queue_drain_all(UReplQueue *q)
{
    if (q == NULL || !q->inited) {
        return NULL;
    }
    pthread_mutex_lock(&q->mutex);
    UReplJob *head = q->head;
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
    pthread_mutex_unlock(&q->mutex);
    return head;
}

UReplJob *
urepl_queue_wait_drain(UReplQueue *q)
{
    if (q == NULL || !q->inited) {
        return NULL;
    }
    pthread_mutex_lock(&q->mutex);
    while (q->head == NULL) {
        pthread_cond_wait(&q->cond_nonempty, &q->mutex);
        /* On shutdown signal, head stays NULL and we exit the loop via
         * the broadcast (caller checks for sentinel head == NULL). */
        if (!q->inited) {
            pthread_mutex_unlock(&q->mutex);
            return NULL;
        }
    }
    UReplJob *head = q->head;
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
    pthread_mutex_unlock(&q->mutex);
    return head;
}

void
urepl_queue_signal_shutdown(UReplQueue *q)
{
    if (q == NULL || !q->inited) {
        return;
    }
    pthread_mutex_lock(&q->mutex);
    /* No new sentinel — consumer re-checks q->inited after waking. */
    pthread_cond_broadcast(&q->cond_nonempty);
    pthread_mutex_unlock(&q->mutex);
}

size_t
urepl_queue_count(UReplQueue *q)
{
    if (q == NULL || !q->inited) {
        return 0;
    }
    pthread_mutex_lock(&q->mutex);
    size_t c = q->count;
    pthread_mutex_unlock(&q->mutex);
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
    if (pthread_mutex_init(&rb->mutex, NULL) != 0) {
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
    pthread_mutex_destroy(&rb->mutex);
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
    pthread_mutex_lock(&rb->mutex);
    size_t written = n;
    if (written > rb->cap) {
        rb->overflow = true;
    }
    ringbuf_write_locked(rb, data, n);
    pthread_mutex_unlock(&rb->mutex);
    return written;
}

size_t
urepl_ringbuf_read(UReplRingbuf *rb, char *dst, size_t dst_cap)
{
    if (rb == NULL || !rb->inited || dst == NULL || dst_cap == 0U) {
        return 0;
    }
    pthread_mutex_lock(&rb->mutex);
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
    pthread_mutex_unlock(&rb->mutex);
    return take;
}

size_t
urepl_ringbuf_fill(UReplRingbuf *rb)
{
    if (rb == NULL || !rb->inited) {
        return 0;
    }
    pthread_mutex_lock(&rb->mutex);
    size_t f = rb->fill;
    pthread_mutex_unlock(&rb->mutex);
    return f;
}

bool
urepl_ringbuf_overflow(UReplRingbuf *rb)
{
    if (rb == NULL || !rb->inited) {
        return false;
    }
    pthread_mutex_lock(&rb->mutex);
    bool o = rb->overflow;
    pthread_mutex_unlock(&rb->mutex);
    return o;
}
