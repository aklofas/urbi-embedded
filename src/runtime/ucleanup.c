/* SPDX-License-Identifier: BSD-3-Clause */
/* Cleanup-stack push/pop helpers and pre-allocated array init/destroy. */

/* Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
   Allocation uses the UVM pluggable allocator (vm->alloc_fn).
   Zero-fill uses a volatile byte loop to prevent compiler lowering to memset.
   Precondition checks use a guarded <assert.h> on hosted targets; on
   freestanding targets the guard expands to a no-op — callers are expected
   to never violate preconditions in production (safety-critical discipline). */

#if __STDC_HOSTED__
#  include <assert.h>
#  define UCLEANUP_ASSERT(x) assert(x)
#else
#  define UCLEANUP_ASSERT(x) ((void)0)
#endif

#include <stddef.h>

#include "runtime/ucleanup.h"
#include "runtime/umacros.h"
#include "sched/ustrand.h"
#include "vm/uvm.h"

UCleanupEntry *
urbi_sched_strand_cleanup_push(struct UStrand *s) {
    UCleanupEntry *e;
    if (s->cleanup_depth >= s->cleanup_cap) return NULL; /* stack full */
    e = &s->cleanup_base[s->cleanup_depth];
    s->cleanup_depth++;
    s->cleanup_top = e;
    return e;
}

void
urbi_sched_strand_cleanup_pop(struct UStrand *s, UCleanupKind expected_kind) {
    UCleanupEntry *top;
    UCLEANUP_ASSERT(s->cleanup_depth > 0);
    top = &s->cleanup_base[s->cleanup_depth - 1];
    UCLEANUP_ASSERT(top->kind == (uint8_t)expected_kind);
    (void)top;          /* suppress unused-variable warning on freestanding */
    (void)expected_kind;
    s->cleanup_depth--;
    s->cleanup_top = (s->cleanup_depth > 0)
        ? &s->cleanup_base[s->cleanup_depth - 1]
        : NULL;
}

int
urbi_sched_strand_cleanup_stack_init(struct UStrand *s, struct UVM *vm, uint16_t cap) {
    /* Pre-allocated, no dynamic growth (row 7 §4.3).
       Uses the VM's pluggable allocator; compatible with freestanding targets. */
    const size_t nbytes = (size_t)cap * sizeof(UCleanupEntry);
    UCleanupEntry *base = (UCleanupEntry *)vm->alloc_fn(NULL, nbytes, vm->alloc_ud);
    if (base == NULL) {
        /* Allocation failure: leave strand in detectable state — caller keeps
           strand in malformed DORMANT and propagates the failure upward. */
        s->cleanup_base  = NULL;
        s->cleanup_cap   = 0;
        s->cleanup_depth = 0;
        s->cleanup_top   = NULL;
        return -1;
    }
    urbi_zero(base, nbytes);
    s->cleanup_base  = base;
    s->cleanup_cap   = cap;
    s->cleanup_depth = 0;
    s->cleanup_top   = NULL;
    return 0;
}

void
urbi_sched_strand_cleanup_stack_destroy(struct UStrand *s, struct UVM *vm) {
    if (s->cleanup_base != NULL) {
        vm->alloc_fn(s->cleanup_base, 0, vm->alloc_ud);
    }
    s->cleanup_base  = NULL;
    s->cleanup_top   = NULL;
    s->cleanup_depth = 0;
    s->cleanup_cap   = 0;
}
