/* SPDX-License-Identifier: BSD-3-Clause */
/* UStrand lifecycle stubs.
   Full initialisation (frame stack, registers, lex env) lands at T20.
   Tag fields land at T29.  Cleanup-stack wiring: T3 (this file). */

#include "ustrand.h"
#include "ucleanup.h"
#include "uvm.h"

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
    strand_zero(s);
    s->vm    = vm;
    s->state = USTRAND_STATE_DORMANT;
    /* Pre-allocate the cleanup stack using the VM's pluggable allocator.
       On allocation failure cleanup_base stays NULL (detectable by caller).
       T20 will add frame-stack / register-window / lex-env init here. */
    if (vm != NULL) {
        (void)strand_cleanup_stack_init(s, vm, URBI_CLEANUP_MAX);
        /* Failure leaves strand in malformed DORMANT (cleanup_base == NULL).
           Caller checks via s->cleanup_base == NULL if needed. */
    }
    /* When vm is NULL the strand has no cleanup stack; callers that omit vm
       must call strand_cleanup_stack_init explicitly before use. */
}

void
ustrand_destroy(UStrand *s, struct UVM *vm) {
    /* Free the pre-allocated cleanup stack if vm is available. */
    if (vm != NULL && s->cleanup_base != NULL) {
        strand_cleanup_stack_destroy(s, vm);
    }
    /* M2 register-window / frame-stack teardown lands at T20. */
}
