/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_auth.c - bearer-token compare + per-IP rate limiter
 *
 * Only compiled when URBI_ENABLE_REPL=1. */
#include "repl/urepl_auth.h"

#include <string.h>

/* ---- Constant-time bearer-token comparison (Task 17) ---------------- */

bool
urepl_auth_token_match(const char *a, size_t alen,
                       const char *b, size_t blen)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    if (alen != blen) {
        return false;
    }
    /* Volatile accumulator: bars the compiler from short-circuiting on
     * the first mismatched byte.  Always walks the full length. */
    volatile unsigned char accum = 0;
    for (size_t i = 0; i < alen; ++i) {
        accum |= (unsigned char)(a[i] ^ b[i]);
    }
    return accum == 0;
}

/* ---- Per-source rate-limiter (Task 18) ------------------------------ */

void
urepl_auth_limiter_init(UReplAuthLimiter *lim)
{
    if (lim == NULL) {
        return;
    }
    memset(lim, 0, sizeof(*lim));
    lim->max_attempts = 5;
    lim->window_secs  = 30;
    lim->lockout_secs = 60;
}

/* Locate the slot owning `ip`, or NULL.  Linear scan; table is small. */
static UReplAuthLimiterSlot *
find_slot(UReplAuthLimiter *lim, uint32_t ip)
{
    size_t cap = sizeof(lim->slots) / sizeof(lim->slots[0]);
    for (size_t i = 0; i < cap; ++i) {
        if (lim->slots[i].in_use && lim->slots[i].ip == ip) {
            return &lim->slots[i];
        }
    }
    return NULL;
}

/* Pick a slot to evict (LRU by last_seen_us; unused slots win). */
static UReplAuthLimiterSlot *
acquire_slot(UReplAuthLimiter *lim, uint32_t ip)
{
    size_t cap = sizeof(lim->slots) / sizeof(lim->slots[0]);
    UReplAuthLimiterSlot *victim = &lim->slots[0];
    for (size_t i = 0; i < cap; ++i) {
        if (!lim->slots[i].in_use) {
            victim = &lim->slots[i];
            break;
        }
        if (lim->slots[i].last_seen_us < victim->last_seen_us) {
            victim = &lim->slots[i];
        }
    }
    memset(victim, 0, sizeof(*victim));
    victim->ip = ip;
    victim->in_use = true;
    return victim;
}

bool
urepl_auth_limiter_check(UReplAuthLimiter *lim, uint32_t ip, uint64_t now_us)
{
    if (lim == NULL) {
        return true;
    }
    if (lim->max_attempts <= 0) {
        /* Limiter disabled — allow. */
        return true;
    }
    UReplAuthLimiterSlot *s = find_slot(lim, ip);
    if (s == NULL) {
        return true;
    }
    /* Lockout window still active? */
    if (s->lockout_until_us > now_us) {
        return false;
    }
    return true;
}

void
urepl_auth_limiter_record_fail(UReplAuthLimiter *lim,
                               uint32_t ip, uint64_t now_us)
{
    if (lim == NULL || lim->max_attempts <= 0) {
        return;
    }
    UReplAuthLimiterSlot *s = find_slot(lim, ip);
    if (s == NULL) {
        s = acquire_slot(lim, ip);
        s->window_start_us = now_us;
    }
    /* Slide the window: if window_secs elapsed since first hit, restart. */
    uint64_t window_span_us = (uint64_t)lim->window_secs * 1000000ULL;
    if (now_us > s->window_start_us
        && now_us - s->window_start_us > window_span_us) {
        s->window_start_us = now_us;
        s->fail_count = 0;
    }
    s->fail_count++;
    s->last_seen_us = now_us;
    if ((int)s->fail_count >= lim->max_attempts) {
        s->lockout_until_us = now_us
            + (uint64_t)lim->lockout_secs * 1000000ULL;
        /* Don't clear fail_count yet; the lockout itself is the gate.
         * After the lockout expires, the next fail bumps fail_count
         * back to 1 because find_slot still returns this slot and the
         * window has elapsed (handled at top of this function). */
    }
}

void
urepl_auth_limiter_record_success(UReplAuthLimiter *lim, uint32_t ip)
{
    if (lim == NULL) {
        return;
    }
    UReplAuthLimiterSlot *s = find_slot(lim, ip);
    if (s != NULL) {
        s->in_use = false;
    }
}
