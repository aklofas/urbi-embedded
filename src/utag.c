/* SPDX-License-Identifier: BSD-3-Clause */
/* UTag lifecycle + ambient-tag scope lookup.
 * Row 11 / T29.
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * Allocation uses vm->alloc_fn (realloc semantics).
 * Zero-fill initializes each field explicitly.
 * Assertions use URBI_INTERNAL_ASSERT from urbi_internal.h. */

#include "utag.h"
#include "ucleanup.h"
#include "ustrand.h"
#include "uvm.h"
#include "ugc.h"       /* UTYPE_TAG */
#include "urbi.h"      /* URBI_ASSERT_NOT_ISR */
#include "urbi_internal.h"

/* === utag_create ===
 *
 * Allocate a UTag via vm->alloc_fn and zero-initialize all fields.
 * type_tag and gc_byte are set explicitly so a future GC migration is a
 * single-line change: replace the alloc with urbi_gc_alloc and remove the
 * manual header init.
 *
 * Returns NULL on OOM. */

UTag *
utag_create(struct UVM *vm)
{
    UTag *tag;

    if (vm == NULL) return NULL;
    URBI_ASSERT_NOT_ISR(vm);

    tag = (UTag *)vm->alloc_fn(NULL, sizeof(UTag), vm->alloc_ud);
    if (tag == NULL) return NULL;

    /* Explicit zero-init of every field (freestanding: no memset). */
    tag->type_tag              = UTYPE_TAG;
    tag->gc_byte               = 0;
    tag->pad0                  = 0;
    tag->flags                 = 0;
    tag->pad1[0]               = 0;
    tag->pad1[1]               = 0;
    tag->pad1[2]               = 0;
    tag->member_strands_head   = NULL;
    tag->member_watchers_head  = NULL;
    tag->name.kind             = UVAL_NIL;
    tag->name.v.i              = 0;

    return tag;
}

/* === utag_destroy ===
 *
 * Free a UTag via vm->alloc_fn.  NULL-safe.
 * Asserts that member lists are empty per §3.5 membership invariant:
 * all TAG_SCOPE entries must have been popped (and thus unlinked) before
 * the tag is destroyed. */

void
utag_destroy(struct UVM *vm, UTag *tag)
{
    if (tag == NULL) return;
    if (vm == NULL) return;
    URBI_ASSERT_NOT_ISR(vm);

    /* §3.5 invariant: member lists must be empty at destroy time. */
    URBI_INTERNAL_ASSERT(tag->member_strands_head  == NULL);
    URBI_INTERNAL_ASSERT(tag->member_watchers_head == NULL);

    vm->alloc_fn(tag, 0, vm->alloc_ud);
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
