/* SPDX-License-Identifier: BSD-3-Clause */
/* UStrand lifecycle stubs.
   Full initialisation (frame stack, registers, lex env) lands at T20.
   Tag fields land at T29.  Cleanup-stack wiring lands at T3. */

#include "ustrand.h"

/* Zero a UStrand without memset — keeps the translation unit freestanding.
   Uses a volatile byte loop (same pattern as arena_zero in uarena.c) so the
   compiler cannot lower it back to a memset libcall under -Os. */
static void strand_zero(UStrand *s) {
    volatile unsigned char *p = (volatile unsigned char *)s;
    size_t n = sizeof(*s);
    size_t i;
    for (i = 0; i < n; i++) p[i] = 0;
}

void
ustrand_init(UStrand *s, struct UVM *vm) {
    (void)vm; /* T20 will use vm to attach the strand to the scheduler */
    strand_zero(s);
    s->state = USTRAND_STATE_DORMANT;
    /* cleanup_base / cleanup_cap set by T3's strand-init helper. */
    /* M2 register-window / frame-stack / lex-env init stays in M2's existing
       strand-init path and will be merged here at T20. */
}

void
ustrand_destroy(UStrand *s) {
    /* T3 will free the cleanup-stack array here.
       M2 register window teardown also lands at T20. */
    (void)s;
}
