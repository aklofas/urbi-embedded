/* SPDX-License-Identifier: BSD-3-Clause */
/* uslot_api.c — public C slot read/write API (Gap K, v0.7.1).
 *
 * urbi_slot_get / urbi_slot_set: host-callable slot access that mirrors
 * the OP_GETSLOT / OP_SETSLOT bytecode dispatch semantics.
 *
 * Dispatch rules:
 *   urbi_slot_get:
 *     UVAL_OBJECT → walk prototype chain via urbi_object_lookup (DFS).
 *     UVAL_INT / FLOAT / STR / BOOL → route through atom-proto via
 *       urbi_atom_proto_for_value (M6 Wave 1 baseline) then walk.
 *     UVAL_NIL / VOID / other atoms → route through atom-proto (NIL/VOID
 *       protos exist since M6 Wave 1).
 *     Not found → URBI_ERR_SLOT_NOT_FOUND.
 *     NULL out_value → URBI_ERR_INVALID_ARG.
 *
 *   urbi_slot_set:
 *     UVAL_OBJECT → install local slot via urbi_object_set_local_slot.
 *       Respects const-flag: URBI_ERR_CONST_SLOT_WRITE if already const.
 *     Any atom (non-OBJECT) → URBI_ERR_INVALID_ARG (atoms are immutable).
 *     OOM → URBI_ERR_OOM.
 *     URBI_OK on success.
 *
 * Freestanding: no <stdlib.h>, <string.h>, <stdio.h>. */

#include "value/uintern.h"          /* ustr_intern */
#include "object/uobject.h"         /* urbi_object_lookup, urbi_object_set_local_slot,
                                       urbi_atom_proto_for_value */
#include "object/ushape.h"          /* URBI_SLOT_FLAG_CONSTANT, urbi_shape_find_slot */
#include "vm/uvm.h"
#include "vm/uvm_error.h"           /* urbi_set_error_internal (Gap P) */
#include "urbi/urbi.h"
#include "urbi/types.h"

#include <stddef.h>
#include <stdint.h>

/* === urbi_slot_get ===
 *
 * Read the value of slot `name[0..name_len)` from receiver `obj`.
 *
 * Returns:
 *   URBI_OK             — *out_value populated.
 *   URBI_ERR_INVALID_ARG — vm or name NULL, or out_value NULL.
 *   URBI_ERR_SLOT_NOT_FOUND — no such slot on the receiver or its prototypes. */
int
urbi_slot_get(struct UVM *vm, UValue obj,
              const char *name, size_t name_len,
              UValue *out_value)
{
    int rc;
    USymbol *sym;
    UObject *recv;
    UValue result;

    if (vm == NULL || name == NULL || out_value == NULL) {
        urbi_set_error_internal(vm, URBI_ERR_INVALID_ARG,
            "urbi_slot_get: vm, name, or out_value is NULL",
            NULL, 0, "urbi_slot_get");
        return URBI_ERR_INVALID_ARG;
    }

    /* Intern the name to get a canonical USymbol* for lookup.
     * ustr_intern returns const char* which aliases USymbol* by convention
     * (USymbol is an opaque forward-struct whose canonical rep is the
     * interned pointer — see umodule.h / uintern.c). */
    sym = (USymbol *)ustr_intern(vm, name, name_len);
    if (sym == NULL) {
        urbi_set_error_internal(vm, URBI_ERR_OOM,
            "urbi_slot_get: OOM interning slot name",
            NULL, 0, "urbi_slot_get");
        return URBI_ERR_OOM;
    }

    /* Resolve the receiver object to walk. For atoms, route through the
     * atom proto so slot lookup mirrors OP_GETSLOT slow-path semantics. */
    recv = urbi_atom_proto_for_value(vm, obj);
    if (recv == NULL) {
        urbi_set_error_internal(vm, URBI_ERR_SLOT_NOT_FOUND,
            "urbi_slot_get: slot not found",
            NULL, 0, "urbi_slot_get");
        return URBI_ERR_SLOT_NOT_FOUND;
    }

    /* Walk the prototype chain. urbi_object_lookup is the cycle-safe DFS
     * walker that respects the left-first prototype ordering.
     * Returns 0 on hit (out populated), -1 on miss or stack overflow. */
    result.kind = (uint8_t)UVAL_NIL;
    result.v.i  = 0;
    rc = urbi_object_lookup(vm, recv, sym, &result);
    if (rc < 0) {
        urbi_set_error_internal(vm, URBI_ERR_SLOT_NOT_FOUND,
            "urbi_slot_get: slot not found",
            NULL, 0, "urbi_slot_get");
        return URBI_ERR_SLOT_NOT_FOUND;
    }

    *out_value = result;
    return URBI_OK;
}

/* === urbi_slot_set ===
 *
 * Write `value` to local slot `name[0..name_len)` on receiver `obj`.
 *
 * Only UVAL_OBJECT receivers are supported — atoms are immutable at the
 * prototype level and cannot receive local slot writes.
 *
 * Const-slot semantics: if the slot already exists on `obj` locally and
 * is flagged CONSTANT, the write is rejected.  If the slot exists only on
 * a prototype (COW case), the COW clone is mutable per pre-M2 §6.1 and
 * the write succeeds.
 *
 * Returns:
 *   URBI_OK                    — slot written.
 *   URBI_ERR_INVALID_ARG       — vm or name NULL, or obj is not UVAL_OBJECT.
 *   URBI_ERR_CONST_SLOT_WRITE  — the slot exists locally and is const.
 *   URBI_ERR_OOM               — intern or slot-array allocation failed. */
int
urbi_slot_set(struct UVM *vm, UValue obj,
              const char *name, size_t name_len,
              UValue value)
{
    if (vm == NULL || name == NULL) {
        urbi_set_error_internal(vm, URBI_ERR_INVALID_ARG,
            "urbi_slot_set: vm or name is NULL",
            NULL, 0, "urbi_slot_set");
        return URBI_ERR_INVALID_ARG;
    }
    /* Atoms (non-OBJECT) do not accept local slot writes. */
    if (obj.kind != (uint8_t)UVAL_OBJECT) {
        urbi_set_error_internal(vm, URBI_ERR_INVALID_ARG,
            "urbi_slot_set: receiver is not an object",
            NULL, 0, "urbi_slot_set");
        return URBI_ERR_INVALID_ARG;
    }

    {
        UObject *recv = (UObject *)obj.v.p;
        USymbol *sym;
        int32_t  local_idx;
        int      rc;

        if (recv == NULL) {
            urbi_set_error_internal(vm, URBI_ERR_INVALID_ARG,
                "urbi_slot_set: object pointer is NULL",
                NULL, 0, "urbi_slot_set");
            return URBI_ERR_INVALID_ARG;
        }

        /* Intern the name (same USymbol* aliasing as urbi_slot_get above). */
        sym = (USymbol *)ustr_intern(vm, name, name_len);
        if (sym == NULL) {
            urbi_set_error_internal(vm, URBI_ERR_OOM,
                "urbi_slot_set: OOM interning slot name",
                NULL, 0, "urbi_slot_set");
            return URBI_ERR_OOM;
        }

        /* Check whether the slot exists locally on `recv` and is const-flagged.
         * Only a LOCAL (same-object) const slot rejects the write; a const slot
         * on a prototype results in a mutable COW copy (pre-M2 §6.1, §8.1). */
        local_idx = urbi_shape_find_slot(recv->shape, sym);
        if (local_idx >= 0) {
            /* Slot exists locally. Check the CONSTANT flag. */
            if (local_idx < 8) {
                uint32_t shift  = (uint32_t)local_idx * 4U;
                uint32_t nibble = (recv->shape->flags >> shift) & 0x0FU;
                if (nibble & URBI_SLOT_FLAG_CONSTANT) {
                    urbi_set_error_internal(vm, URBI_ERR_CONST_SLOT_WRITE,
                        "urbi_slot_set: slot is const",
                        NULL, 0, "urbi_slot_set");
                    return URBI_ERR_CONST_SLOT_WRITE;
                }
            }
            /* Local mutable slot: in-place update via set_local_slot. */
        }
        /* For new slots (local_idx < 0) or inherited slots (COW install):
         * urbi_object_set_local_slot handles both cases correctly.
         * For existing local mutable slots, it does an in-place update. */
        rc = urbi_object_set_local_slot(vm, recv, sym, value);
        if (rc != 0) {
            urbi_set_error_internal(vm, URBI_ERR_OOM,
                "urbi_slot_set: OOM allocating slot",
                NULL, 0, "urbi_slot_set");
            return URBI_ERR_OOM;
        }
    }
    return URBI_OK;
}
