/* SPDX-License-Identifier: BSD-3-Clause */
/* UTag lifecycle + ambient-tag scope lookup.
 * Row 11 / T29; GC-promoted at M5 T18.
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * Allocation uses urbi_gc_alloc (GC-managed from birth, M5 T18).
 * Zero-fill initializes each field explicitly.
 * Assertions use URBI_INTERNAL_ASSERT from umacros.h. */

#include "utag.h"
#include "ucleanup.h"
#include "ustrand.h"
#include "uvm.h"
#include "gc/ugc.h"       /* UTYPE_TAG */
#include "urbi/gc.h"      /* urbi_gc_alloc */
#include "urbi/urbi.h"    /* URBI_ASSERT_NOT_ISR */
#include "umacros.h"

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
    /* urbi_gc_alloc zeroes the allocation and sets gc_byte = current_white.
     * Set identity and payload fields explicitly. */
    tag->type_tag              = UTYPE_TAG;
    tag->pad0                  = 0;
    tag->flags                 = 0;
    tag->pad1[0]               = 0;
    tag->pad1[1]               = 0;
    tag->pad1[2]               = 0;
    tag->member_strands_head   = NULL;
    tag->member_watchers_head  = NULL;
    tag->enter_event           = NULL;
    tag->leave_event           = NULL;
    tag->name.kind             = UVAL_NIL;
    tag->name.v.i              = 0;

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
utag_destroy(struct UVM *vm, UTag *tag)
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
