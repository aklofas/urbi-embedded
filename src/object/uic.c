/* SPDX-License-Identifier: BSD-3-Clause */
/* uic.c — IC slow-path helpers (urbi_slot_get_slow / urbi_slot_set_slow).
 *
 * Spec references:
 *   docs/superpowers/specs/2026-04-29-urbi-pre-m4-getslot-setslot-encoding-design.md §6.3
 *   docs/superpowers/specs/2026-04-29-urbi-pre-m4-uslot-uprops-collapse-design.md §4.2, §5.1
 *   docs/superpowers/specs/2026-04-24-urbi-pre-m2-object-model-design.md §6.1, §8.1
 *
 * Two entry points called from the OP_GETSLOT / OP_SETSLOT dispatch arms
 * when the inline-cache fast path misses (no shape+topology match).  Both
 * resolve the slot via urbi_object_resolve_slot (DFS over the prototype
 * graph) and fill exactly one IC entry at ic->replace_cursor.  After fill
 * the dispatch arm inspects ic->flags[fresh_k] to dispatch a getter/setter
 * if needed; the helpers themselves never call into the VM.
 *
 * COW semantics for SET (per pre-M2 §6.1): if the slot resolves on a
 * prototype (holder != recv), install a fresh local slot on recv with the
 * new value, leaving the prototype's slot intact.
 *
 * Return codes:
 *   urbi_slot_get_slow → 0 on success (out_value populated unless the
 *                       caller will invoke a getter — see §6.1).  -1 on
 *                       miss or resolve-stack overflow.
 *   urbi_slot_set_slow → URBI_OK on success.  Distinct codes per OBJ-009 +
 *                       API-007: URBI_ERR_CONST_SLOT_WRITE on constant
 *                       overwrite (formerly collapsed onto -1),
 *                       URBI_ERR_OOM on COW / set_local_slot allocation
 *                       failure, URBI_ERR_INVALID_ARG for NULL args, and
 *                       generic -1 for resolve-stack overflow / future
 *                       internal failure paths. */

#include "object/uic.h"
#include "object/uobject.h"
#include "object/ushape.h"
#include "vm/uvm.h"
#include "urbi/types.h"   /* URBI_ERR_CONST_SLOT_WRITE / URBI_ERR_OOM (OBJ-009) */

#include <stddef.h>
#include <stdint.h>

/* Compute the per-IC flag byte for the slot at `idx` in `holder`'s shape:
 *   - low 4 bits = the per-slot flag nibble packed into UShape.flags
 *   - URBI_SLOT_FLAG_LOCAL is set only if holder == recv (i.e. the slot
 *     lives on the receiver, not an inherited prototype).
 * UShape.flags packs 4 bits per slot in a 32-bit word → valid for indices
 * 0..7 only (8 * 4 = 32 bits).  Indices >= 8 carry no packed flags at the
 * v1.0 baseline (a side-table tier for larger objects is deferred to a later
 * milestone); return 0 for those to avoid undefined-behaviour shifts. */
static uint8_t
ic_flags_for_resolved_slot(const UObject *recv, const UObject *holder,
                           uint32_t idx)
{
    uint8_t flags = 0U;
    if (idx < 8U) {
        flags = (uint8_t)((holder->shape->flags >> (idx * 4U)) & 0x0FU);
    }
    if (holder == recv) {
        flags |= URBI_SLOT_FLAG_LOCAL;
    }
    return flags;
}

/* Per-slot UProps lookup; NULL if the holder shape carries no UPropsTable
 * (the common case at v1.0 — UPropsTable is only allocated when at least
 * one property has been installed in the lineage). */
static UProps *
ic_uprops_for_resolved_slot(const UObject *holder, uint32_t idx)
{
    if (holder->shape->props_table == NULL) {
        return NULL;
    }
    return holder->shape->props_table[idx];
}

/* Fill exactly one entry of `ic` at the round-robin replace_cursor with the
 * resolved slot information.  Advances replace_cursor and grows ic->n until
 * the cache is full.  Hoisted from static to header-declared at OBJ-033
 * (Wave 6 cleanup) to enable future megamorphic-bail call sites without
 * a follow-up edit. */
void
ic_fill_at_cursor(UIC *ic, const UVM *vm, const UObject *recv,
                  UObject *holder, uint32_t idx, UProps *up, uint8_t flags)
{
    uint8_t k = ic->replace_cursor;
    ic->recv_shapes[k]  = recv->shape;
    ic->topology_gen[k] = vm->topology_gen;
    ic->slots[k]        = &holder->slots[idx];
    ic->uprops[k]       = up;
    /* OBJ-IC-POLY: record idx for the LOCAL-slot fast path so polymorphic
     * same-shape receivers re-resolve their own slot rather than reading the
     * first cached recv's slot.  Inherited slots (holder is a stable proto)
     * keep using the absolute `slots[k]` pointer.  uint16_t supports indices
     * 0..65535 — far beyond any realistic v1.0 class layout (eye_demo's
     * Color has 5 slots).  Indices >= 65536 would saturate at 0xFFFF and
     * the fast path would dereference the wrong slot; flag-clear the LOCAL
     * bit defensively to force slow-path in that improbable case. */
    if (idx < 0xFFFFU) {
        ic->slot_idx[k] = (uint16_t)idx;
    } else {
        ic->slot_idx[k] = 0xFFFFU;
        flags &= (uint8_t)~URBI_SLOT_FLAG_LOCAL;  /* poison: bypass fast path */
    }
    ic->flags[k]        = flags;
    ic->replace_cursor  = (uint8_t)((k + 1U) % URBI_IC_ENTRIES_PER_SITE);
    /* Grow live-entry count up to the cap.  When k == ic->n the cache is
     * still warming; once n hits URBI_IC_ENTRIES_PER_SITE the cursor wraps
     * and we overwrite the round-robin victim in place. */
    if (ic->n < URBI_IC_ENTRIES_PER_SITE && k >= ic->n) {
        ic->n = (uint8_t)(k + 1U);
    }
}

/* === urbi_slot_get_slow ===
 *
 * IC miss for OP_GETSLOT.  Resolve via prototype walk, fill the IC entry
 * at the replace cursor, and either:
 *   - leave the OGET-flag set so the caller dispatches the getter, OR
 *   - copy the slot value to *out_value for the caller to publish to R[A].
 *
 * Returns 0 on success, -1 on miss or resolve overflow. */
int
urbi_slot_get_slow(UVM *vm, UObject *recv, UIC *ic, UValue *out_value)
{
    /* v0.8.2 bring-up debug: always-on tracer (4 prior calls succeeded; the
     * 5th hangs).  Each call emits ~6 markers -- ~30 UART lines total.
     * Remove before tag. */
#define SGDBG(s) do { if (vm && vm->writer_fn)                             \
    vm->writer_fn(vm->writer_ud, "sg", 2, s, sizeof(s)-1U, 0); } while (0)

    SGDBG("enter\n");
    if (vm == NULL || recv == NULL || ic == NULL
        || ic->name == NULL || out_value == NULL) {
        SGDBG("INVALID ARG\n");
        return -1;
    }

    UObject *holder = NULL;
    uint32_t idx    = 0U;
    SGDBG("pre resolve_slot\n");
    int rc = urbi_object_resolve_slot(vm, recv, ic->name, &holder, &idx);
    SGDBG("post resolve_slot\n");
    if (rc <= 0) {
        /* 0 == miss; -1 == resolve-stack overflow.  T40 will land the
         * fallback retry; for now both surface as "not found" to caller. */
        SGDBG("resolve miss\n");
        return -1;
    }
    SGDBG("pre uprops_for_resolved\n");
    UProps  *up    = ic_uprops_for_resolved_slot(holder, idx);
    SGDBG("pre flags_for_resolved\n");
    uint8_t  flags = ic_flags_for_resolved_slot(recv, holder, idx);
    SGDBG("pre ic_fill_at_cursor\n");
    ic_fill_at_cursor(ic, vm, recv, holder, idx, up, flags);
    SGDBG("post ic_fill_at_cursor\n");

    if (flags & URBI_SLOT_FLAG_OGET) {
        /* Caller (OP_GETSLOT slow path) inspects the just-filled flags and
         * invokes URBI_VM_DISPATCH_GETTER.  Don't write *out_value. */
        SGDBG("OGET return\n");
        return 0;
    }

    SGDBG("publish out_value\n");
    *out_value = holder->slots[idx];
    SGDBG("return 0\n");
    return 0;
#undef SGDBG
}

/* === urbi_slot_set_slow ===
 *
 * IC miss for OP_SETSLOT.  Three resolved cases:
 *   (a) miss          → install a fresh local slot on recv (no IC fill —
 *                       leaf-shape-add invalidates the IC's shape match
 *                       on the next access naturally).
 *   (b) holder == recv (local hit on a path the IC didn't cache):
 *       - CONSTANT     → URBI_ERR_CONST_SLOT_WRITE
 *       - OSET         → fill IC; caller dispatches setter
 *       - otherwise    → in-place write + IC fill
 *   (c) holder != recv (resolved on a prototype):
 *       - OSET         → fill IC pointing at holder's slot; caller
 *                        dispatches setter (per pre-M2 §8.1: setter on
 *                        the prototype's slot fires)
 *       - otherwise    → COW: install a local slot on recv with the new
 *                        value (no IC fill — the receiver's shape just
 *                        transitioned, so the next access misses by shape
 *                        and the slow path re-resolves to the local copy).
 *                        The source's CONSTANT flag does NOT carry through
 *                        to the COW'd local slot — const-ness binds the
 *                        source slot, not derivations (touchstone: legacy
 *                        slot-cow-const.chk; T57).
 *
 * OBJ-009: distinct error codes — URBI_OK on success, URBI_ERR_INVALID_ARG
 * for NULL args, URBI_ERR_CONST_SLOT_WRITE on constant overwrite,
 * URBI_ERR_OOM on COW / set_local_slot allocation failure, generic -1
 * (resolve-stack overflow / future error paths) for other internal
 * failures.  Callers that previously checked `rc != 0` continue to work. */
int
urbi_slot_set_slow(UVM *vm, UObject *recv, UIC *ic, UValue value)
{
    if (vm == NULL || recv == NULL || ic == NULL || ic->name == NULL) {
        return URBI_ERR_INVALID_ARG;
    }

    UObject *holder = NULL;
    uint32_t idx    = 0U;
    int rc = urbi_object_resolve_slot(vm, recv, ic->name, &holder, &idx);

    if (rc < 0) {
        return -1;   /* resolve-stack overflow */
    }
    if (rc == 0) {
        /* Slot doesn't exist anywhere on the chain: install on recv.
         * urbi_object_set_local_slot handles the leaf-shape-add transition
         * + USlotArray growth; no IC fill needed because the new shape
         * naturally fails the next IC shape-match. */
        if (urbi_object_set_local_slot(vm, recv, ic->name, value) != 0) {
            return URBI_ERR_OOM;
        }
        return URBI_OK;
    }

    /* rc == 1 — resolved at (holder, idx). */
    UProps  *up    = ic_uprops_for_resolved_slot(holder, idx);
    uint8_t  flags = ic_flags_for_resolved_slot(recv, holder, idx);

    if (flags & URBI_SLOT_FLAG_OSET) {
        /* Setter dispatch: fill the IC pointing at holder's slot so the
         * fast path next access dispatches the setter without re-walking.
         * Caller inspects flags[fresh_k] and invokes URBI_VM_DISPATCH_SETTER. */
        ic_fill_at_cursor(ic, vm, recv, holder, idx, up, flags);
        return URBI_OK;
    }

    if (holder == recv) {
        /* Local slot on the receiver: const-ness is binding here. */
        if (flags & URBI_SLOT_FLAG_CONSTANT) {
            /* Constant slot on the receiver — write rejected per pre-M2
             * §8.1.  No IC fill (would just reject again on the next
             * access).  OBJ-009 / API-007: distinct from OOM. */
            return URBI_ERR_CONST_SLOT_WRITE;
        }
        /* In-place write on the local slot.  Fill the IC so subsequent
         * writes hit the fast path. */
        recv->slots[idx] = value;
        ic_fill_at_cursor(ic, vm, recv, holder, idx, up, flags);
        return URBI_OK;
    }

    /* holder != recv — slot resolves on a prototype.  COW per pre-M2
     * §6.1: install a fresh local slot on recv with the new value.  The
     * SOURCE slot's CONSTANT flag does NOT carry through: const-ness is
     * an attribute of the source slot, not a constraint on derivations
     * (touchstone: legacy slot-cow-const.chk — `var b = a.new(); b.x =
     * 12;` succeeds even when a.x is const).  The new local slot is
     * mutable; the prototype's slot is untouched.
     *
     * No IC fill — the receiver's shape just transitioned (or, if recv
     * already had `name` locally, find_slot in resolve would have hit
     * recv first); subsequent accesses miss by shape and the slow path
     * resolves to the new local copy. */
    if (urbi_object_set_local_slot(vm, recv, ic->name, value) != 0) {
        return URBI_ERR_OOM;
    }
    return URBI_OK;
}
