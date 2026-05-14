/* SPDX-License-Identifier: BSD-3-Clause */
/* utag_api.c — public C tag manipulation API (Gap M, v0.7.1).
 *
 * urbi_tag_create: allocate a GC-managed UTag, intern its name, and set
 *   its parent to the realm's root tag.  Returns NULL on OOM.
 *
 * urbi_tag_info: populate a urbi_tag_info_t snapshot from a live UTag.
 *   Reads flags, walks member_strands_head, checks parent != NULL.
 *   Returns URBI_OK or URBI_ERR_INVALID_ARG.
 *
 * Freestanding: no <stdlib.h>, <string.h>, <stdio.h>.
 *
 * Drift-detection asserts for urbi_tag_state_t ↔ UTAG_FLAG_* mapping
 * live here (not in the public header) because urbi.h must not include
 * internal src/ headers. */

#include "tag/utag.h"           /* UTag, utag_create, UTAG_FLAG_*, member_strands_head */
#include "runtime/ucleanup.h"   /* UCleanupEntry, next_member */
#include "realm/urealm.h"       /* URealm, realm->tag */
#include "value/uintern.h"      /* ustr_intern */
#include "vm/uvm.h"
#include "urbi/urbi.h"          /* urbi_tag_create, urbi_tag_info, urbi_tag_state_t,
                                   urbi_tag_info_t, URBI_OK, URBI_ERR_INVALID_ARG */
#include "runtime/umacros.h"    /* URBI_ASSERT_NOT_ISR */
#include "gc/ugc_incremental.h" /* gc_shade_gray (Dijkstra forward barrier) */
#include "urbi/gc.h"            /* urbi_gc_slot_write */

#include <stddef.h>
#include <stdint.h>

/* Drift-detection: if the UTAG_FLAG_* bit positions change, the mapping
 * in urbi_tag_info (flags → urbi_tag_state_t) must be updated. */
_Static_assert(UTAG_FLAG_FROZEN  == 0x01U,
               "urbi_tag_state_t assumes UTAG_FLAG_FROZEN == 0x01");
_Static_assert(UTAG_FLAG_STOPPED == 0x02U,
               "urbi_tag_state_t assumes UTAG_FLAG_STOPPED == 0x02");

/* === urbi_tag_create ===
 *
 * Allocate a UTag via utag_create, intern the name into tag->name, and
 * set tag->parent = realm->tag (the realm's root tag).
 *
 * The parent pointer is shaded by the UTYPE_TAG GC walker in utypes_init.c
 * (walk_utag), so the parent remains reachable as long as the child is live.
 *
 * Returns NULL on OOM or if vm/realm is NULL. */
struct UTag *
urbi_tag_create(struct UVM *vm, struct URealm *realm,
                const char *name, size_t name_len)
{
    if (vm == NULL || realm == NULL) {
        return NULL;
    }
    URBI_ASSERT_NOT_ISR(vm);

    /* Allocate the UTag cell.  utag_create sets type_tag=UTYPE_TAG and
     * zero-fills all payload fields (gc_alloc contract: urbi_gc_alloc
     * zeroes the allocation). */
    UTag *tag = utag_create(vm);
    if (tag == NULL) {
        return NULL;
    }

    /* Intern the name if provided.  Empty or NULL name leaves name.kind=NIL. */
    if (name != NULL && name_len > 0U) {
        const char *interned = ustr_intern(vm, name, name_len);
        if (interned == NULL) {
            /* OOM on intern.  The UTag cell is GC-managed — it will be swept
             * when it becomes unreachable.  Return NULL to signal OOM. */
            return NULL;
        }
        UValue name_val;
        name_val.kind = (uint8_t)UVAL_STR;
        name_val.v.p  = (void *)interned;
        /* Write via GC barrier: tag is a fresh white cell under a possibly-black
         * realm-root, so shade the name value to maintain tri-color invariant. */
        urbi_gc_slot_write(vm, (UCell *)tag, 0U, name_val);
        tag->name = name_val;
    }

    /* Parent under realm's root tag.  All host-created tags are children of
     * the realm tag so urbi_tag_info can report has_parent = true.
     * Shade the parent via the forward barrier (realm->tag is reachable
     * through the realm GC root; the write here pins the reverse reference). */
    if (realm->tag != NULL) {
        gc_shade_gray(vm, (UCell *)realm->tag);
        tag->parent = realm->tag;
    }

    return tag;
}

/* === urbi_tag_info ===
 *
 * Populate *out with a snapshot of `tag`'s observable state:
 *   state        — derived from tag->flags (RUNNING/STOPPED/FROZEN/BLOCKED).
 *   member_count — number of UCleanupEntry nodes on member_strands_head.
 *   has_parent   — tag->parent != NULL.
 *
 * Returns URBI_OK on success, URBI_ERR_INVALID_ARG if tag or out is NULL. */
int
urbi_tag_info(const struct UTag *tag, urbi_tag_info_t *out)
{
    if (tag == NULL || out == NULL) {
        return URBI_ERR_INVALID_ARG;
    }

    /* Decode state from flags.  Priority: STOPPED > FROZEN > RUNNING.
     * BLOCKED is reserved (no flag bit at v1.0). */
    if (tag->flags & UTAG_FLAG_STOPPED) {
        out->state = URBI_TAG_STOPPED;
    } else if (tag->flags & UTAG_FLAG_FROZEN) {
        out->state = URBI_TAG_FROZEN;
    } else {
        out->state = URBI_TAG_RUNNING;
    }

    /* Walk member_strands_head to count attached strands. */
    size_t count = 0U;
    const struct UCleanupEntry *e = tag->member_strands_head;
    while (e != NULL) {
        count++;
        e = e->next_member;
    }
    out->member_count = count;

    /* has_parent: true if the parent pointer was set by urbi_tag_create. */
    out->has_parent = (tag->parent != NULL);

    return URBI_OK;
}
