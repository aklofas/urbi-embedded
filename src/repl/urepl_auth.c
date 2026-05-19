/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_auth.c - bearer-token compare + per-IP rate limiter
 *
 * Only compiled when URBI_ENABLE_REPL=1. */
#include "repl/urepl_auth.h"

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
