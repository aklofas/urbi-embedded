/* SPDX-License-Identifier: BSD-3-Clause */
/* uobject_lookup.c — cycle-safe DFS slot lookup.
 * Extracted from uobject.c during v0.5.4-decompose (OBJ-045 #3). */

#include <stdint.h>

#include "object/uobject.h"
#include "object/uobject_internal.h"
#include "object/ushape.h"
#include "vm/uvm.h"
#include "value/uintern.h"       /* ustr_intern (fallback retry) */
#include "gc/ugc_incremental.h"  /* urbi_gc_walk_all_cells */
#include "gc/ugc.h"              /* UTYPE_OBJECT */
#include "chunk/umodule.h"
#include <stddef.h>

/* === T12: cycle-safe DFS lookup ===
 *
 * Per pre-M4 prototype-chain spec §6 + GETSLOT/SETSLOT spec §6.5.
 *
 * The visited-set is encoded by stamping UObject.lookup_stamp with the low
 * 32 bits of vm->lookup_id; a re-visit (stamp == lookup_id) short-circuits.
 * Each top-level urbi_object_lookup call pre-bumps lookup_id, guaranteeing
 * the new id is fresh against every UObject's previous stamp.
 *
 * urbi_shape_find_slot is a stub at T12 (always returns -1); T13 lands the
 * real lineage walk.  Until then lookup_inner unconditionally falls into
 * the proto-walk path on every visit — which is exactly what the cycle and
 * rollover tests need to exercise. */

int
lookup_inner(UVM *vm, UObject *obj, USymbol *name, UValue *out)
{
    /* Cycle / re-visit guard.  Truncating lookup_id to u32 is intentional —
     * UObject.lookup_stamp is 4 bytes per spec §3 to keep the header at
     * 48 B.  Rollover is handled by urbi_object_lookup pre-checking the
     * top-level bump. */
    if (obj->lookup_stamp == (uint32_t)vm->lookup_id) {
        return 0;   /* already visited; not found via this branch */
    }
    obj->lookup_stamp = (uint32_t)vm->lookup_id;

    /* Local-slot fast path.  T12: urbi_shape_find_slot is a stub returning
     * -1, so this branch is never taken yet — obj->slots may be NULL.
     * T13 lands the real find; T26 lands slot-array growth that makes
     * obj->slots non-NULL once the first slot transitions in. */
    const UShape *s = obj->shape;
    int32_t idx = urbi_shape_find_slot(s, name);
    if (idx >= 0) {
        *out = obj->slots[idx];
        return 1;
    }

    /* Proto-walk: left-first DFS via UPROTOS_FOREACH (visits items[0]
     * first per spec §6.1).  Recursion depth is bounded by the proto
     * graph depth; cycles are short-circuited by the lookup_stamp guard
     * at function entry. */
    UObject *p;
    UPROTOS_FOREACH(obj, p) {
        int rc = lookup_inner(vm, p, name, out);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

int
urbi_object_lookup(UVM *vm, UObject *obj, USymbol *name, UValue *out)
{
    /* Rollover check: if the next u32 truncation of (lookup_id + 1) would
     * be 0 (the "no stamp" sentinel), force a clear-pass and reset to 1
     * BEFORE doing the increment.  Otherwise pre-bump so the new id is
     * fresh against every UObject's previous stamp.
     *
     * OBJ-014: track whether this entry already triggered a force_wrap.
     * If so, the second-pass fallback retry can safely bump lookup_id
     * without re-checking the wrap condition (force_wrap left lookup_id
     * at 1 and cleared every UObject's stamp, so a second bump to id=2
     * is unconditionally fresh).  This avoids a redundant walk-all-cells
     * pass in the wrap-during-first-pass scenario. */
    int wrapped_in_first_pass = 0;
    /* CPPCHK-002: cppcheck static analysis flags this as always-false because
     * it assumes a 32-bit lookup_id; vm->lookup_id is uint64_t and the cast
     * catches the actual u32 wrap.  Suppressed via .cppcheck.suppressions. */
    if ((uint32_t)(vm->lookup_id + 1ULL) == 0U) {
        urbi_object_lookup_id_force_wrap(vm);
        wrapped_in_first_pass = 1;
        /* force_wrap leaves lookup_id == 1, which is fresh after the
         * just-cleared stamps. */
    } else {
        vm->lookup_id++;
    }

    int rc = lookup_inner(vm, obj, name, out);
    if (rc == 1) {
        return 0;   /* hit */
    }

    /* T40: GET_FALLBACK retry per pre-M2 §4.3.  On full-tree miss, retry
     * once with name = "fallback".  If fallback hits, return its value;
     * the caller is responsible for invoking it as a method.  v1.0 does
     * NOT implement the legacy `call.message` reflection layer (per
     * third-party-corpus-compatibility-audit B disposition); fallback
     * receives the bare value and must dispatch via plain function args.
     *
     * Cycle-safety: if `name` itself is "fallback", skip the retry —
     * otherwise lookup of "fallback" missing on an object without one
     * would recurse forever.  Bump lookup_id again so the second pass
     * uses a fresh stamp against UObjects that were marked during the
     * first pass; rollover-guard mirrors the entry path EXCEPT when the
     * first pass already triggered a wrap (cf. OBJ-014). */
    USymbol *fb = (USymbol *)ustr_intern(vm, "fallback", 8);
    if (name == fb) {
        return -1;
    }
    /* OBJ-014: skip the wrap-check on the second pass when force_wrap
     * already fired in the first pass — stamps were cleared and
     * lookup_id is currently >= 1, so a plain pre-bump is unconditionally
     * fresh.  Otherwise apply the standard rollover guard. */
    /* CPPCHK-002: same u32 rollover guard as the first-pass check above —
     * suppressed via .cppcheck.suppressions; live correctness on 64-bit. */
    if (!wrapped_in_first_pass
        && (uint32_t)(vm->lookup_id + 1ULL) == 0U) {
        urbi_object_lookup_id_force_wrap(vm);
    } else {
        vm->lookup_id++;
    }
    rc = lookup_inner(vm, obj, fb, out);
    return (rc == 1) ? 0 : -1;
}

/* clear_lookup_stamp_cb — urbi_gc_walk_all_cells callback that resets
 * UObject.lookup_stamp to 0 on every UObject cell.  Skips non-object cells. */
void
clear_lookup_stamp_cb(UVM *vm, UCell *cell, void *ctx)
{
    (void)vm; (void)ctx;
    if (cell->type_tag == UTYPE_OBJECT) {
        ((UObject *)cell)->lookup_stamp = 0U;
    }
}

void
urbi_object_lookup_id_force_wrap(UVM *vm)
{
    /* Walk every GC cell, zero lookup_stamp on UObject cells.  T36 may
     * fold this into the mark phase to avoid a separate iteration; for
     * now an immediate dedicated pass is correct (per spec §7.2). */
    urbi_gc_walk_all_cells(vm, clear_lookup_stamp_cb, NULL);
    vm->lookup_id = 1ULL;
}
