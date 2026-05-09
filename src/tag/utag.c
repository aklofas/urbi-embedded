/* SPDX-License-Identifier: BSD-3-Clause */
/* UTag lifecycle + ambient-tag scope lookup.
 * Row 11 / T29; GC-promoted at M5 T18.
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * Allocation uses urbi_gc_alloc (GC-managed from birth, M5 T18).
 * Zero-fill initializes each field explicitly.
 * Assertions use URBI_INTERNAL_ASSERT from umacros.h. */

#include "tag/utag.h"
#include "runtime/ucleanup.h"
#include "sched/ustrand.h"
#include "vm/uvm.h"
#include "gc/ugc.h"       /* UTYPE_TAG */
#include "urbi/gc.h"      /* urbi_gc_alloc */
#include "urbi/urbi.h"    /* URBI_ASSERT_NOT_ISR */
#include "runtime/umacros.h"
#include <stddef.h>

/* === utag_create ===
 *
 * Allocate a UTag via urbi_gc_alloc and zero-initialize payload fields.
 * urbi_gc_alloc zeroes the whole allocation and sets gc_byte = current_white.
 * We set the identity fields explicitly after allocation.
 *
 * Returns NULL on OOM. */

UTag *
utag_create(struct UVM *vm)
{
    UCell *c;
    UTag  *tag;

    if (vm == NULL) return NULL;
    URBI_ASSERT_NOT_ISR(vm);

    c = urbi_gc_alloc(vm, sizeof(UTag), UTYPE_TAG);
    if (c == NULL) return NULL;

    tag = (UTag *)c;
    /* TAGCH-003: urbi_gc_alloc owns the zero-init contract — it zeros every
     * byte of the allocation via urbi_zero (see ugc_incremental.c::urbi_gc_alloc)
     * and then writes type_tag and gc_byte = current_white.  Every UTag field
     * other than the cell-header bytes is therefore already zero, which is the
     * desired payload state (NULL pointers, flags = 0, name.kind = UVAL_NIL = 0).
     * No explicit zero loop is needed; verified by tests/unit/test_utag_gc.c
     * (utag_gc_alloc_zero_init_contract). */

    return tag;
}

/* === utag_destroy ===
 *
 * Assert the §3.5 membership invariant (member lists must be empty).
 * Does NOT free the UTag memory — the GC sweep reclaims GC-managed cells.
 * NULL-safe.
 *
 * Called from OP_POP_TAG, urbi_realm_destroy, and rollback paths in
 * urbi_realm_create and OP_PUSH_TAG.  All callers must ensure member lists
 * are empty before calling (strand_unlink_from_tags / pending_onleave_queue
 * handle unlinking). */

void
utag_destroy(struct UVM *vm, const UTag *tag)
{
    if (tag == NULL) return;
    if (vm == NULL) return;

    /* §3.5 invariant: member lists must be empty at destroy time. */
    URBI_INTERNAL_ASSERT(tag->member_strands_head  == NULL);
    URBI_INTERNAL_ASSERT(tag->member_watchers_head == NULL);

    /* GC-managed: the GC sweep reclaims the cell; no alloc_fn free here. */
    (void)vm;
}

/* === urbi_strand_scope_tag ===
 *
 * Walk the strand's cleanup-stack top-down (depth-1 → 0) and return the
 * owning_tag of the innermost TAG_SCOPE entry found, or NULL if none.
 *
 * Read-only — does not modify any state.  ISR-safe from a data-race
 * perspective (strand must be quiescent while inspecting; not multi-threaded
 * at M3 so the constraint is trivially met).
 *
 * Spec §3.8 specifies: "unreachable in practice — realm->tag is at the bottom".
 * We return NULL explicitly to handle empty cleanup-stacks safely (e.g. in
 * tests that construct bare strands without ambient tags). */

UTag *
urbi_strand_scope_tag(struct UStrand *s)
{
    int i;

    if (s == NULL || s->cleanup_depth == 0 || s->cleanup_base == NULL)
        return NULL;

    for (i = (int)s->cleanup_depth - 1; i >= 0; i--) {
        UCleanupEntry *e = &s->cleanup_base[i];
        if (e->kind == UCLEANUP_TAG_SCOPE)
            return e->owning_tag;
    }
    return NULL;
}
