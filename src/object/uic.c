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
 *   urbi_slot_set_slow → 0 on success.  -1 on COW-OOM, constant-write,
 *                       or resolve-stack overflow. */

#include "object/uic.h"
#include "object/uobject.h"
#include "object/ushape.h"
#include "vm/uvm.h"

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
    uint8_t flags = 0u;
    if (idx < 8u) {
        flags = (uint8_t)((holder->shape->flags >> (idx * 4u)) & 0x0Fu);
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
 * the cache is full. */
static void
ic_fill_at_cursor(UIC *ic, UVM *vm, const UObject *recv,
                  UObject *holder, uint32_t idx, UProps *up, uint8_t flags)
{
    uint8_t k = ic->replace_cursor;
    ic->recv_shapes[k]  = recv->shape;
    ic->topology_gen[k] = vm->topology_gen;
    ic->slots[k]        = &holder->slots[idx];
    ic->uprops[k]       = up;
    ic->flags[k]        = flags;
    ic->replace_cursor  = (uint8_t)((k + 1u) % URBI_IC_ENTRIES_PER_SITE);
    /* Grow live-entry count up to the cap.  When k == ic->n the cache is
     * still warming; once n hits URBI_IC_ENTRIES_PER_SITE the cursor wraps
     * and we overwrite the round-robin victim in place. */
    if (ic->n < URBI_IC_ENTRIES_PER_SITE && k >= ic->n) {
        ic->n = (uint8_t)(k + 1u);
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
    if (vm == NULL || recv == NULL || ic == NULL
        || ic->name == NULL || out_value == NULL) {
        return -1;
    }

    UObject *holder = NULL;
    uint32_t idx    = 0u;
    int rc = urbi_object_resolve_slot(vm, recv, ic->name, &holder, &idx);
    if (rc <= 0) {
        /* 0 == miss; -1 == resolve-stack overflow.  T40 will land the
         * fallback retry; for now both surface as "not found" to caller. */
        return -1;
    }

    UProps  *up    = ic_uprops_for_resolved_slot(holder, idx);
    uint8_t  flags = ic_flags_for_resolved_slot(recv, holder, idx);
    ic_fill_at_cursor(ic, vm, recv, holder, idx, up, flags);

    if (flags & URBI_SLOT_FLAG_OGET) {
        /* Caller (OP_GETSLOT slow path) inspects the just-filled flags and
         * invokes URBI_VM_DISPATCH_GETTER.  Don't write *out_value. */
        return 0;
    }

    *out_value = holder->slots[idx];
    return 0;
}

/* === urbi_slot_set_slow ===
 *
 * IC miss for OP_SETSLOT.  Three resolved cases:
 *   (a) miss          → install a fresh local slot on recv (no IC fill —
 *                       leaf-shape-add invalidates the IC's shape match
 *                       on the next access naturally).
 *   (b) holder == recv (local hit on a path the IC didn't cache):
 *       - CONSTANT     → -1 (constant slot)
 *       - OSET         → fill IC; caller dispatches setter
 *       - otherwise    → in-place write + IC fill
 *   (c) holder != recv (resolved on a prototype):
 *       - OSET         → fill IC pointing at holder's slot; caller
 *                        dispatches setter (per pre-M2 §8.1: setter on
 *                        the prototype's slot fires)
 *       - otherwise    → COW: install a local slot on recv with the new
 *                        value (no IC fill — the receiver's shape just
 *                        transitioned, so the next access misses by shape
 *                        and the slow path re-resolves to the local copy)
 *
 * Returns 0 on success, -1 on COW-OOM / constant-write / resolve overflow. */
int
urbi_slot_set_slow(UVM *vm, UObject *recv, UIC *ic, UValue value)
{
    if (vm == NULL || recv == NULL || ic == NULL || ic->name == NULL) {
        return -1;
    }

    UObject *holder = NULL;
    uint32_t idx    = 0u;
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
            return -1;   /* OOM */
        }
        return 0;
    }

    /* rc == 1 — resolved at (holder, idx). */
    UProps  *up    = ic_uprops_for_resolved_slot(holder, idx);
    uint8_t  flags = ic_flags_for_resolved_slot(recv, holder, idx);

    if (flags & URBI_SLOT_FLAG_OSET) {
        /* Setter dispatch: fill the IC pointing at holder's slot so the
         * fast path next access dispatches the setter without re-walking.
         * Caller inspects flags[fresh_k] and invokes URBI_VM_DISPATCH_SETTER. */
        ic_fill_at_cursor(ic, vm, recv, holder, idx, up, flags);
        return 0;
    }

    if (flags & URBI_SLOT_FLAG_CONSTANT) {
        /* Constant slot — write rejected.  Whether the slot lives on the
         * receiver or a prototype, attempting to write a CONSTANT slot is
         * an error per pre-M2 §8.1.  No IC fill (would just reject again
         * on the next access). */
        return -1;
    }

    if (holder == recv) {
        /* In-place write on the local slot.  Fill the IC so subsequent
         * writes hit the fast path. */
        recv->slots[idx] = value;
        ic_fill_at_cursor(ic, vm, recv, holder, idx, up, flags);
        return 0;
    }

    /* COW: holder != recv and no setter / not constant.  Install a local
     * slot on recv with the new value; the prototype's slot is untouched.
     * No IC fill — the receiver's shape just transitioned (or, if recv
     * already had `name` locally, find_slot in resolve would have hit
     * recv first); subsequent accesses will miss by shape and the slow
     * path will resolve to the new local copy. */
    if (urbi_object_set_local_slot(vm, recv, ic->name, value) != 0) {
        return -1;
    }
    return 0;
}
