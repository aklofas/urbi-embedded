/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_auth.h - bearer-token compare + per-IP rate limiter
 *
 * Task 17 lands the constant-time compare; Task 18 adds the LRU
 * rate-limit table.  Both are pure-functional + thread-unsafe — the
 * caller (server) wraps them in pthread_mutex_t auth_limiter_mutex.
 *
 * Only compiled when URBI_ENABLE_REPL=1. */
#ifndef SRC_REPL_UREPL_AUTH_H
#define SRC_REPL_UREPL_AUTH_H

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

#ifdef __cplusplus
}
#endif

#endif /* SRC_REPL_UREPL_AUTH_H */
