/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_auth.h - bearer-token compare + per-IP rate limiter
 *
 * Task 17 lands the constant-time compare; Task 18 adds the LRU
 * rate-limit table.  Both are pure-functional + thread-unsafe — the
 * caller (server) wraps them in pthread_mutex_t auth_limiter_mutex.
 *
 * Only compiled when URBI_ENABLE_REPL=1. */
#ifndef UREPL_AUTH_H
#define UREPL_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Constant-time bearer-token comparison.
 *
 * Returns true iff alen == blen AND all bytes equal.  Either arg NULL
 * → false; either length zero → only matches the other being zero.
 *
 * Implementation:  volatile-marked accumulator XORs every byte; the
 * compiler is barred from short-circuiting on the first mismatch.  The
 * spec mandates this to deny timing-side-channel guesses of the bearer
 * token (spec §7.3). */
bool urepl_auth_token_match(const char *a, size_t alen,
                            const char *b, size_t blen);

/* ---- Per-source rate-limiter (Task 18) ------------------------------ */

/* Fixed-size LRU table.  Each slot tracks one peer (ip for TCP, pid
 * for Unix-socket) with a sliding window of failed-auth attempts.
 * After `max_attempts` failures within `window_secs`, the slot enters
 * lockout state for `lockout_secs`; check returns false during lockout.
 *
 * Default tunables (spec §7.4): 5 / 30s / 60s.  Caller fills these
 * fields explicitly when constructing the limiter (zero-defaults
 * collapse to "no rate limit" → check always returns true).
 *
 * 8 slots is enough for v0.9.1's typical deployment (one or two
 * editor clients per dev machine); LRU eviction means a fast IP-
 * churn attack just rolls the table, which doesn't hurt the legitimate
 * client whose slot is preserved while it's actively authing. */
typedef struct UReplAuthLimiterSlot {
    uint32_t ip;                    /* peer identifier (TCP: in_addr_t; Unix: pid) */
    uint32_t fail_count;            /* hits in current window */
    uint64_t window_start_us;       /* first hit's host time */
    uint64_t last_seen_us;          /* most-recent hit; LRU eviction key */
    uint64_t lockout_until_us;      /* > now ⇒ locked out */
    bool     in_use;
} UReplAuthLimiterSlot;

typedef struct UReplAuthLimiter {
    UReplAuthLimiterSlot slots[8];
    int max_attempts;     /* default 5 */
    int window_secs;      /* default 30 */
    int lockout_secs;     /* default 60 */
} UReplAuthLimiter;

/* Initialize default tunables (5 / 30 / 60).  Zeroes the slots. */
void urepl_auth_limiter_init(UReplAuthLimiter *lim);

/* Returns true if the source is permitted to connect/auth now.
 * Returns false if `ip` is currently locked out. */
bool urepl_auth_limiter_check(UReplAuthLimiter *lim,
                              uint32_t ip, uint64_t now_us);

/* Record an auth failure for `ip` at `now_us`.  Updates the slot
 * (creating one if needed via LRU eviction) and transitions to
 * lockout state when fail_count hits max_attempts within the window. */
void urepl_auth_limiter_record_fail(UReplAuthLimiter *lim,
                                    uint32_t ip, uint64_t now_us);

/* Clear any record for `ip` (called on successful auth). */
void urepl_auth_limiter_record_success(UReplAuthLimiter *lim, uint32_t ip);

#ifdef __cplusplus
}
#endif

#endif /* UREPL_AUTH_H */
