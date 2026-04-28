/* SPDX-License-Identifier: BSD-3-Clause */
/* UStrand lifecycle.
   T20 adds urbi_strand_create/start/spawn/destroy (full lifecycle C API).
   Tag fields land at T29.  Cleanup-stack wiring: T3 (this file). */

#include "ustrand.h"
#include "ucleanup.h"
#include "uvm.h"
#include "urealm.h"
#include "usched_cooperative.h"
#include "urbi.h"
#include "urbi_internal.h"

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
    /* register-window / frame-stack teardown: not yet needed (execution state
       is zero-init at T20; frame-0 setup via urbi_step or a future urbi_strand_arm). */
}

/* === T20: Strand C API (create / start / spawn / destroy) ===
 *
 * These functions implement the strand lifecycle as specified in row 9 §9.1.
 * Separate _create (DORMANT alloc) from _start (DORMANT → READY enqueue) so
 * callers can pre-attach tags (T29), set scheduler attrs (v1.x), or pool/recycle
 * strands before making them runnable.  _spawn is the convenience composite. */

UStrand *
urbi_strand_create(struct URealm *realm, struct UClosure *entry)
{
    struct UVM *vm = realm->vm;
    URBI_ASSERT_NOT_ISR(vm);

    /* Allocate via VM pluggable allocator (no stdlib calloc — freestanding). */
    UStrand *s = (UStrand *)vm->alloc_fn(NULL, sizeof(UStrand), vm->alloc_ud);
    if (!s) return NULL;

    /* Zero-initialize without memset (freestanding: no <string.h>).
       Uses a volatile byte loop so the compiler cannot lower it to a memset libcall. */
    {
        volatile unsigned char *p = (volatile unsigned char *)s;
        size_t n = sizeof(UStrand);
        size_t i;
        for (i = 0; i < n; i++) p[i] = 0;
    }

    ustrand_init(s, vm);
    s->realm         = realm;
    s->entry_closure = entry;

    /* T29: per row 11 §4, chunk-start ambient = [realm->tag] via
       urbi_strand_attach_ambient_tags.  At M3 realm->tag is always NULL
       (set to NULL at T14 as the M3 stub), so this block would be a no-op
       even if the function existed.  Deferred cleanly. */

    sched_strand_init(s, NULL);

    /* Execution state (stack, R, pc, etc.) is zero-init; subsequent
       activation by urbi_step or a future urbi_strand_arm helper sets up
       frame-0 from entry_closure. */

    /* state stays USTRAND_DORMANT — sched_strand_init does not change state. */
    return s;
}

void
urbi_strand_start(UStrand *s)
{
    struct UVM *vm = s->vm;
    URBI_ASSERT_NOT_ISR(vm);
    (void)vm;  /* suppress -Wunused-variable in non-debug builds */
    URBI_INTERNAL_ASSERT(USTRAND_GET_STATE(s) == USTRAND_DORMANT);
    sched_strand_make_runnable(s);
}

UStrand *
urbi_strand_spawn(struct URealm *realm, struct UClosure *entry)
{
    UStrand *s = urbi_strand_create(realm, entry);
    if (s) urbi_strand_start(s);
    return s;
}

void
urbi_strand_destroy(UStrand *s)
{
    struct UVM *vm;
    if (!s) return;
    vm = s->vm;
    if (vm) URBI_ASSERT_NOT_ISR(vm);
    sched_strand_destroy(s);
    ustrand_destroy(s, vm);
    if (vm) vm->alloc_fn(s, 0, vm->alloc_ud);
}
