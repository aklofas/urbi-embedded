/* SPDX-License-Identifier: BSD-3-Clause */
/* uobject_proto.c — prototype-mutation primitives.
 * Extracted from uobject.c during v0.5.4-decompose (OBJ-045 #2). */

#include <stdint.h>

#include "object/uobject.h"
#include "object/uobject_internal.h"
#include "vm/uvm.h"
#include "urbi/gc.h"           /* urbi_gc_alloc */
#include "gc/ugc_incremental.h" /* gc_shade_gray */
#include "gc/ugc.h"            /* UTYPE_PROTOS */
#include "urbi/urbi.h"         /* URBI_OK / URBI_ERR_INVALID_ARG */
#include <stddef.h>

/* Cap on the number of distinct prototypes a single setProtos call may
 * install (after dedup).  Stays in sync with the plan's stack-array sizing;
 * a larger cap can land in v1.x as part of stdlib error-code expansion. */
#define URBI_PROTOS_SETPROTOS_CAP  64U

/* shade_existing_protos — internal helper. Decodes obj->protos's three
 * storage forms (empty/single/heap per spec §4.1) and shades the underlying
 * cell(s) before the field is overwritten. */
void
shade_existing_protos(UVM *vm, UObject *obj)
{
    uintptr_t raw = obj->protos;
    if (raw == 0U) {
        return;   /* empty form — nothing to shade */
    }
    if ((raw & 1U) != 0U) {
        /* single form: bit 0 set, address in high bits — UProtos pointer-
         * encoding (TIDY-003 design pin per pre-M4 prototype-chain spec §7.2). */
        gc_shade_gray(vm, (UCell *)(raw >> 1));  /* NOLINT(performance-no-int-to-ptr) — UProtos single-form pointer-encoding */
    } else {
        /* heap form: raw is a UProtos*. Shade the UProtos cell itself.
         * The UObject*s in items[] are reachable from the UProtos walker
         * (utypes_init.c walk_uprotos), so shading the UProtos is sufficient
         * to keep them alive across the overwrite — the GC will trace into
         * items[] when it next dequeues this gray cell. */
        gc_shade_gray(vm, (UCell *)raw);  /* NOLINT(performance-no-int-to-ptr) — UProtos heap-form pointer-encoding */
    }
}

void
urbi_object_set_protos_empty(UVM *vm, UObject *obj)
{
    shade_existing_protos(vm, obj);
    obj->protos = 0U;
    vm->topology_gen++;
}

void
urbi_object_set_protos_single(UVM *vm, UObject *obj, UObject *p)
{
    shade_existing_protos(vm, obj);
    /* Forward barrier on the inserted child (per spec §5.3 — barrier is
     * per-write, not per-disposition). */
    gc_shade_gray(vm, (UCell *)p);
    obj->protos = ((uintptr_t)p << 1) | 1U;
    /* T27: mark the inserted prototype so future slot installs on it bump
     * topology_gen (topology spec §4.1 row 4).  Monotonic — never cleared. */
    p->flags |= URBI_OBJ_FLAG_IS_PROTOTYPE;
    vm->topology_gen++;
}

void
urbi_object_set_protos_heap(UVM *vm, UObject *obj, UProtos *up)
{
    shade_existing_protos(vm, obj);
    /* Shade every item in the new UProtos plus the UProtos cell itself —
     * pre-write barriers on inserted children (per spec §5.3). */
    for (uint32_t i = 0; i < up->n; i++) {
        gc_shade_gray(vm, (UCell *)up->items[i]);
        /* T27: mark each prototype.  Monotonic (see _single above). */
        up->items[i]->flags |= URBI_OBJ_FLAG_IS_PROTOTYPE;
    }
    gc_shade_gray(vm, (UCell *)up);
    obj->protos = (uintptr_t)up;
    vm->topology_gen++;
}

/* valid_proto — atom-family compatibility check per pre-M4 prototype-chain
 * spec §5.5.  An atom can only inherit from its own family OR from the
 * root Object atom.  The root Object never blocks (either side may be
 * URBI_ATOM_OBJECT and the relationship is permitted). */
int
valid_proto(const UObject *obj, const UObject *p)
{
    URBIAtomFamily ofam = (URBIAtomFamily)(obj->flags & URBI_OBJ_ATOM_MASK);
    URBIAtomFamily pfam = (URBIAtomFamily)(p->flags   & URBI_OBJ_ATOM_MASK);
    if (ofam == URBI_ATOM_OBJECT || pfam == URBI_ATOM_OBJECT) {
        return 1;
    }
    return ofam == pfam;
}

/* urbi_protos_alloc — allocate a fresh UProtos block for `n` items via the
 * GC (UTYPE_PROTOS).  Caller fills items[0..n).  Returns NULL on OOM. */
UProtos *
urbi_protos_alloc(UVM *vm, uint32_t n)
{
    UCell *c = urbi_gc_alloc(vm,
                             sizeof(UProtos) + (size_t)n * sizeof(UObject *),
                             UTYPE_PROTOS);
    if (c == NULL) {
        return NULL;
    }
    UProtos *up = (UProtos *)c;
    up->n    = n;
    up->_pad = 0U;
    for (uint32_t i = 0; i < n; i++) {
        up->items[i] = NULL;
    }
    return up;
}

int
urbi_object_add_proto(struct UVM *vm, UObject *obj, UObject *proto)
{
    if (vm == NULL || obj == NULL || proto == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    if (!valid_proto(obj, proto)) {
        return URBI_ERR_INVALID_ARG;
    }

    /* Prepend at index 0 per pre-M2 §5.1: most-recently-added prototype
     * gets MRO priority. */
    uint32_t old_n = urbi_object_proto_count(obj);

    if (old_n == 0U) {
        urbi_object_set_protos_single(vm, obj, proto);
        return URBI_OK;
    }

    /* old_n >= 1 — build a fresh UProtos with [proto, ...existing]. */
    uint32_t new_n = old_n + 1U;
    if (new_n > URBI_PROTOS_SETPROTOS_CAP) {
        return URBI_ERR_INVALID_ARG;
    }
    UProtos *up = urbi_protos_alloc(vm, new_n);
    if (up == NULL) {
        return URBI_ERR_OOM;
    }
    up->items[0] = proto;
    for (uint32_t i = 0; i < old_n; i++) {
        up->items[i + 1U] = urbi_object_proto_at(obj, i);
    }
    urbi_object_set_protos_heap(vm, obj, up);
    return URBI_OK;
}

int
urbi_object_remove_proto(struct UVM *vm, UObject *obj, const UObject *proto)
{
    if (vm == NULL || obj == NULL || proto == NULL) {
        return URBI_ERR_INVALID_ARG;
    }

    uint32_t old_n = urbi_object_proto_count(obj);

    /* Find first occurrence; silent no-op if absent (legacy semantics per
     * pre-M2 §5.2). */
    uint32_t idx = old_n;   /* sentinel "not found" */
    for (uint32_t i = 0; i < old_n; i++) {
        if (urbi_object_proto_at(obj, i) == proto) {
            idx = i;
            break;
        }
    }
    if (idx == old_n) {
        return URBI_OK;   /* not present — silent no-op */
    }

    uint32_t new_n = old_n - 1U;
    if (new_n == 0U) {
        urbi_object_set_protos_empty(vm, obj);
        return URBI_OK;
    }
    if (new_n == 1U) {
        /* Pick the survivor (the one element whose index isn't `idx`). */
        UObject *survivor = urbi_object_proto_at(obj, (idx == 0U) ? 1U : 0U);
        urbi_object_set_protos_single(vm, obj, survivor);
        return URBI_OK;
    }

    /* new_n >= 2: build a fresh UProtos skipping idx. */
    UProtos *up = urbi_protos_alloc(vm, new_n);
    if (up == NULL) {
        return URBI_ERR_OOM;
    }
    uint32_t out = 0U;
    for (uint32_t i = 0; i < old_n; i++) {
        if (i == idx) continue;
        up->items[out++] = urbi_object_proto_at(obj, i);
    }
    urbi_object_set_protos_heap(vm, obj, up);
    return URBI_OK;
}

int
urbi_object_set_protos(struct UVM *vm, UObject *obj, UObject **list, uint32_t n)
{
    if (vm == NULL || obj == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    if (n > 0U && list == NULL) {
        return URBI_ERR_INVALID_ARG;
    }

    /* Dedup first-occurrence-wins onto a stack array; cap at
     * URBI_PROTOS_SETPROTOS_CAP distinct survivors (per plan).  Skip NULL
     * entries up-front — they are invalid prototype slots. */
    UObject *deduped[URBI_PROTOS_SETPROTOS_CAP];
    uint32_t dn = 0U;
    for (uint32_t i = 0; i < n; i++) {
        UObject *cand = list[i];
        if (cand == NULL) {
            return URBI_ERR_INVALID_ARG;
        }
        /* O(dn) duplicate check — dn bounded by 64 so this is cheap. */
        int dup = 0;
        for (uint32_t j = 0; j < dn; j++) {
            if (deduped[j] == cand) { dup = 1; break; }
        }
        if (dup) continue;
        if (dn >= URBI_PROTOS_SETPROTOS_CAP) {
            return URBI_ERR_INVALID_ARG;   /* over cap */
        }
        deduped[dn++] = cand;
    }

    /* Validate every survivor BEFORE mutating any state — atomicity per the
     * plan's "no partial state" requirement. */
    for (uint32_t i = 0; i < dn; i++) {
        if (!valid_proto(obj, deduped[i])) {
            return URBI_ERR_INVALID_ARG;
        }
    }

    /* All checks passed; dispatch on dedup count. */
    if (dn == 0U) {
        urbi_object_set_protos_empty(vm, obj);
        return URBI_OK;
    }
    if (dn == 1U) {
        urbi_object_set_protos_single(vm, obj, deduped[0]);
        return URBI_OK;
    }
    UProtos *up = urbi_protos_alloc(vm, dn);
    if (up == NULL) {
        return URBI_ERR_OOM;
    }
    for (uint32_t i = 0; i < dn; i++) {
        up->items[i] = deduped[i];
    }
    urbi_object_set_protos_heap(vm, obj, up);
    return URBI_OK;
}
