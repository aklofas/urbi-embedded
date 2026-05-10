/* SPDX-License-Identifier: BSD-3-Clause */
/* uvm_op_overload.c — operator-method fallback dispatch (Gap #4, M6 Wave 3).
 *
 * When an arith_* call returns a type error (e.g., Object + Object where
 * neither is a numeric atom), the dispatch loop tries to find an operator-
 * named slot on the lhs proto chain and call it as a method.  This provides
 * operator overloading for user-defined types compatible with the legacy
 * urbiscript convention (slot name is the bare operator string: "+", "-",
 * etc.).
 *
 * For OP_EQ / OP_NEQ the situation differs: uvalue_equal never raises a type
 * error — it silently returns false for incompatible types.  The fallback
 * gate for those opcodes is: the lhs is a UVAL_OBJECT (user type).
 *
 * IC: the UOpOverloadIC table in the UVM struct caches the (pc_offset,
 * topology_gen) → UClosure* mapping per call site so the proto-chain walk
 * is elided on subsequent calls at the same site.  IC invalidation follows
 * the same topology_gen bump points as the existing OP_GETSLOT IC. */

#include "vm/uvm_op_overload.h"

#include <stddef.h>
#include <stdint.h>

#include "vm/uvm.h"
#include "vm/uvm_internal.h"
#include "object/uobject.h"
#include "runtime/uclosure.h"
#include "value/uintern.h"
#include "sched/ustrand.h"
#include "watcher/uwatcher.h"   /* urbi_run_closure_on_scratch[_with_payload] */

/* -----------------------------------------------------------------------
 * IC helpers
 * --------------------------------------------------------------------- */

/* Map a pc_offset to a site index via modulo.  Simple and collision-tolerant:
 * two opcodes at different pc offsets that map to the same site slot simply
 * evict each other (IC still correct; just misses more often). */
static uint32_t site_index(uint32_t pc_off)
{
    return pc_off % (uint32_t)URBI_OP_OVERLOAD_IC_SITES;
}

/* Look up the IC for (pc_offset, op_name).  Returns a UClosure* on hit,
 * NULL on miss.  A hit requires both pc_offset match AND topology_gen match
 * AND op_name pointer equality (interned, so pointer == content). */
static struct UClosure *
ic_lookup(UVM *vm, uint32_t pc_off, const USymbol *op_name)
{
    uint32_t si = site_index(pc_off);
    UOpOverloadIC *ic = &vm->op_overload_ic;
    uint8_t n = ic->n[si];
    for (uint8_t k = 0; k < n; k++) {
        UOpOverloadICEntry *e = &ic->entries[si][k];
        if (e->pc_offset    == pc_off
         && e->op_name      == op_name
         && e->topology_gen == vm->topology_gen
         && e->cached       != NULL) {
            return e->cached;
        }
    }
    return NULL;
}

/* Fill one IC entry at the round-robin cursor for (pc_offset, op_name, cl). */
static void
ic_fill(UVM *vm, uint32_t pc_off, USymbol *op_name, struct UClosure *cl)
{
    uint32_t si = site_index(pc_off);
    UOpOverloadIC *ic = &vm->op_overload_ic;
    uint8_t cur = ic->cursor[si];
    UOpOverloadICEntry *e = &ic->entries[si][cur];
    e->pc_offset    = pc_off;
    e->topology_gen = vm->topology_gen;
    e->op_name      = op_name;
    e->cached       = cl;
    /* Advance cursor (round-robin eviction). */
    ic->cursor[si] = (uint8_t)((cur + 1U) % URBI_OP_OVERLOAD_IC_ENTRIES_PER_SITE);
    if (ic->n[si] < (uint8_t)URBI_OP_OVERLOAD_IC_ENTRIES_PER_SITE) {
        ic->n[si]++;
    }
}

/* -----------------------------------------------------------------------
 * Slot resolution helper
 * --------------------------------------------------------------------- */

/* Resolve the operator-named slot on `recv_obj`'s proto chain.
 * Returns a UClosure* if found and it is a bytecode or native closure.
 * Returns NULL if not found, not a closure, or OOM during lookup.
 * Caches the result in the IC table keyed by pc_off. */
static struct UClosure *
resolve_op_closure(UVM *vm, UObject *recv_obj,
                   USymbol *op_name, uint32_t pc_off)
{
    /* IC fast path. */
    struct UClosure *cached = ic_lookup(vm, pc_off, op_name);
    if (cached != NULL) {
        return cached;
    }

    /* Slow path: proto-chain walk. */
    UValue v;
    int rc = urbi_object_lookup(vm, recv_obj, op_name, &v);
    if (rc != 0) {
        return NULL;   /* miss or error */
    }
    if (v.kind != (uint8_t)UVAL_CLOSURE || v.v.p == NULL) {
        return NULL;   /* found but not a closure */
    }

    struct UClosure *cl = (struct UClosure *)v.v.p;
    ic_fill(vm, pc_off, op_name, cl);
    return cl;
}

/* -----------------------------------------------------------------------
 * Public helpers: binary and unary fallback
 * --------------------------------------------------------------------- */

/* vm_arith_method_fallback — binary operator fallback.
 *
 * Called when arith_add/sub/mul/div returns a type error and the lhs is a
 * user object.  Looks up `op_name` slot on lhs's proto chain; if found,
 * calls it with `rhs` as the sole argument and places the result in `*dst`.
 *
 * Returns VM_OP_OVERLOAD_OK if the call succeeded (dst is populated).
 * Returns VM_OP_OVERLOAD_MISS if no usable slot was found (caller should
 * emit the original type error and HALT).
 * Returns VM_OP_OVERLOAD_OOM on allocation failure during lookup/call. */
int
vm_arith_method_fallback(UVM *vm,
                         UValue *dst,
                         const UValue *lhs,
                         const UValue *rhs,
                         USymbol *op_name,
                         uint32_t pc_off)
{
    if (lhs->kind != (uint8_t)UVAL_OBJECT || lhs->v.p == NULL) {
        return VM_OP_OVERLOAD_MISS;
    }

    UObject *recv_obj = (UObject *)lhs->v.p;
    struct UClosure *cl = resolve_op_closure(vm, recv_obj, op_name, pc_off);
    if (cl == NULL) {
        return VM_OP_OVERLOAD_MISS;
    }

    /* Dispatch: native closure → direct call; bytecode closure → scratch run.
     *
     * Native path: same as OP_CALL's native-fn arm.  Receiver is lhs
     * (published to vm->last_recv for symmetry with OP_GETSLOT → OP_CALL).
     * Args: rhs as args[0].
     *
     * Bytecode path: urbi_run_closure_on_scratch_with_payload writes rhs into
     * R[0] of the scratch strand.  The function's nparams is typically 1 for
     * binary ops (function(other){...}); the scratch runner does not enforce
     * nparams, so a 0-param body also runs (and ignores the payload). */
    UValue result;
    if (cl->native_fn != NULL) {
        UValue arg = *rhs;
        vm->last_recv = *lhs;
        int nrc = cl->native_fn(vm, *lhs, &arg, 1, &result);
        if (nrc != 0) {
            return VM_OP_OVERLOAD_MISS;
        }
    } else {
        int threw = 0;
        int src = urbi_run_closure_on_scratch_with_payload(vm, cl, *rhs,
                                                           &result, &threw);
        if (src != 0 || threw) {
            return VM_OP_OVERLOAD_MISS;
        }
    }

    *dst = result;
    return VM_OP_OVERLOAD_OK;
}

/* vm_arith_method_fallback_unary — unary operator fallback.
 *
 * Same as the binary variant but no rhs argument.  Used by OP_NEG.
 * The slot should have nparams=0: `function() { ... }`. */
int
vm_arith_method_fallback_unary(UVM *vm,
                               UValue *dst,
                               const UValue *operand,
                               USymbol *op_name,
                               uint32_t pc_off)
{
    if (operand->kind != (uint8_t)UVAL_OBJECT || operand->v.p == NULL) {
        return VM_OP_OVERLOAD_MISS;
    }

    UObject *recv_obj = (UObject *)operand->v.p;
    struct UClosure *cl = resolve_op_closure(vm, recv_obj, op_name, pc_off);
    if (cl == NULL) {
        return VM_OP_OVERLOAD_MISS;
    }

    UValue result;
    if (cl->native_fn != NULL) {
        vm->last_recv = *operand;
        int nrc = cl->native_fn(vm, *operand, NULL, 0, &result);
        if (nrc != 0) {
            return VM_OP_OVERLOAD_MISS;
        }
    } else {
        int threw = 0;
        int src = urbi_run_closure_on_scratch(vm, cl, &result, &threw);
        if (src != 0 || threw) {
            return VM_OP_OVERLOAD_MISS;
        }
    }

    *dst = result;
    return VM_OP_OVERLOAD_OK;
}

/* vm_cmp_method_fallback — comparison operator fallback for OP_EQ / OP_NEQ.
 *
 * Unlike arith ops, uvalue_equal does not raise a type error — it just
 * returns false.  We intercept when lhs is a user object, look up the
 * "==" or "!=" slot, and call it.  The result is coerced to a bool:
 * truthy (non-zero int, non-nil) → true, nil/void/0 → false.
 *
 * Returns VM_OP_OVERLOAD_OK and writes result bool to *out_bool on success.
 * Returns VM_OP_OVERLOAD_MISS if no slot found (caller uses uvalue_equal). */
int
vm_cmp_method_fallback(UVM *vm,
                       bool *out_bool,
                       const UValue *lhs,
                       const UValue *rhs,
                       USymbol *op_name,
                       uint32_t pc_off)
{
    if (lhs->kind != (uint8_t)UVAL_OBJECT || lhs->v.p == NULL) {
        return VM_OP_OVERLOAD_MISS;
    }

    UObject *recv_obj = (UObject *)lhs->v.p;
    struct UClosure *cl = resolve_op_closure(vm, recv_obj, op_name, pc_off);
    if (cl == NULL) {
        return VM_OP_OVERLOAD_MISS;
    }

    UValue result;
    if (cl->native_fn != NULL) {
        UValue arg = *rhs;
        vm->last_recv = *lhs;
        int nrc = cl->native_fn(vm, *lhs, &arg, 1, &result);
        if (nrc != 0) {
            return VM_OP_OVERLOAD_MISS;
        }
    } else {
        int threw = 0;
        int src = urbi_run_closure_on_scratch_with_payload(vm, cl, *rhs,
                                                           &result, &threw);
        if (src != 0 || threw) {
            return VM_OP_OVERLOAD_MISS;
        }
    }

    /* Coerce result to bool.  int/bool non-zero → true; nil/void → false. */
    if (result.kind == (uint8_t)UVAL_INT) {
        *out_bool = (result.v.i != 0);
    } else if (result.kind == (uint8_t)UVAL_BOOL) {
        *out_bool = (result.v.i != 0);
    } else if (result.kind == (uint8_t)UVAL_NIL
            || result.kind == (uint8_t)UVAL_VOID) {
        *out_bool = false;
    } else {
        /* Non-nil non-int → truthy. */
        *out_bool = true;
    }

    return VM_OP_OVERLOAD_OK;
}
