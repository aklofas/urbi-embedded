/* SPDX-License-Identifier: BSD-3-Clause */
/* uvm_op_overload.c — operator-method fallback dispatch (Gap #4).
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
 * topology_gen, recv_shape, recv_protos) → (holder, slot_idx) slot
 * LOCATION per call site so the proto-chain walk is elided on subsequent
 * calls at the same site.  The hit path re-reads the live slot — the IC
 * never holds a closure pointer (refactor-3 GC-06/VM-06c: a cached
 * closure VALUE went stale on in-place slot overwrite, which deliberately
 * does not bump topology_gen, and dangled once GC swept the replaced
 * closure).  The receiver dimension (recv_shape + recv_protos word) keys
 * polymorphic sites: without it, a second receiver of a different class
 * at the same pc silently dispatched the first class's operator.
 * IC invalidation follows the same topology_gen bump points as the
 * existing OP_GETSLOT IC, plus the receiver and holder-shape matches. */

#include "vm/uvm_op_overload.h"

#include <stddef.h>
#include <stdint.h>

#include "vm/uvm.h"
#include "vm/uvm_internal.h"
#include "object/uobject.h"
#include "runtime/uclosure.h"
#include "value/uintern.h"
#include "sched/ustrand.h"
#include "gc/ugc_incremental.h"  /* urbi_c_root_push/_pop (v0.13.2 native out-slot rooting) */
#include "runtime/umacros.h"     /* urbi_zero */
#include "runtime/uscratch.h"   /* urbi_run_closure_on_scratch[_with_payload|_ex] */
#include "vm/uvm_reactive_drain.h"  /* vm_reactive_drain (VM-20: drain after operator call) */

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

/* Look up the IC for (recv_obj, pc_offset, op_name).  Returns a UClosure*
 * on hit, NULL on miss.  A hit requires pc_offset match AND topology_gen
 * match AND op_name pointer equality (interned, so pointer == content)
 * AND the receiver dimension: recv_shape pins the receiver's local-slot
 * layout, recv_protos (opaque word compare, never dereferenced) pins the
 * proto-list identity — shape alone cannot discriminate the receiver's
 * class because fresh instances of slot-less classes all share the root
 * shape.  Same-shape + same-protos different-identity receivers sharing
 * one entry is CORRECT (identical resolution; slot UIC semantics).
 * The entry caches WHERE the slot lives (holder or live receiver,
 * slot_idx), never the closure value (refactor-3 GC-06/VM-06c): the hit
 * path re-reads the live slot below. */
static struct UClosure *
ic_lookup(UVM *vm, const UObject *recv_obj,
          uint32_t pc_off, const USymbol *op_name)
{
    if (vm->op_overload_ic == NULL) return NULL;   /* IC not allocated (OOM at init) */
    uint32_t si = site_index(pc_off);
    UOpOverloadIC *ic = vm->op_overload_ic;
    uint8_t n = ic->n[si];
    for (uint8_t k = 0; k < n; k++) {
        UOpOverloadICEntry *e = &ic->entries[si][k];
        if (e->pc_offset    == pc_off
         && e->op_name      == op_name
         && e->topology_gen == vm->topology_gen
         && e->recv_shape   == recv_obj->shape
         && e->recv_protos  == recv_obj->protos) {
            /* Re-read the live slot: in-place overwrites (no gen bump by
             * design, topology spec §4.2 row 2) and GC-replaced closures
             * are both picked up.  Bounds safety: the recv/holder shape
             * match guarantees slot_idx is valid for the slot array read
             * below — the index was resolved against that exact shape at
             * fill time, and any slot add/remove transitions the owner to
             * a different UShape.  Slot no longer a closure → slow path. */
            UValue v;
            if ((e->flags & URBI_OPIC_FLAG_LOCAL) != 0U) {
                /* OBJ-IC-POLY mirror: a local slot is receiver-specific —
                 * re-resolve via the LIVE receiver (recv_shape matched, so
                 * slot_idx is valid for recv_obj->slots[]); the fill-time
                 * receiver may be a different same-shape instance. */
                v = recv_obj->slots[e->slot_idx];
            } else {
                if (e->holder == NULL
                 || e->holder->shape != e->holder_shape) {
                    return NULL;   /* stale holder layout → slow path */
                }
                v = e->holder->slots[e->slot_idx];
            }
            if (v.kind == (uint8_t)UVAL_CLOSURE && v.v.p != NULL) {
                return (struct UClosure *)v.v.p;
            }
            return NULL;
        }
    }
    return NULL;
}

/* Fill one IC entry at the round-robin cursor for (recv_obj, pc_offset,
 * op_name) with the slot LOCATION the slow path resolved — not the
 * closure value (refactor-3 GC-06).  A slot local to the receiver
 * (holder == recv_obj) is stored as URBI_OPIC_FLAG_LOCAL + slot_idx only;
 * the hit path re-resolves through the live receiver (OBJ-IC-POLY
 * mirror) so no per-instance pointer is retained.
 *
 * Holder lifetime (inherited case): e->holder / e->holder_shape are raw
 * pointers with the same lifetime assumption the slot UIC makes
 * (object/uic.h caches recv_shapes/slots identically) — proto-chain
 * holders are realm-rooted for the life of the type.  This parity is
 * deliberate; see the design-risks register. */
static void
ic_fill(UVM *vm, const UObject *recv_obj, uint32_t pc_off,
        USymbol *op_name, UObject *holder, uint16_t slot_idx)
{
    if (vm->op_overload_ic == NULL) return;   /* IC not allocated; skip fill */
    uint32_t si = site_index(pc_off);
    UOpOverloadIC *ic = vm->op_overload_ic;
    uint8_t cur = ic->cursor[si];
    UOpOverloadICEntry *e = &ic->entries[si][cur];
    e->pc_offset    = pc_off;
    e->topology_gen = vm->topology_gen;
    e->op_name      = op_name;
    e->recv_shape   = recv_obj->shape;
    e->recv_protos  = recv_obj->protos;
    if (holder == recv_obj) {
        e->flags        = URBI_OPIC_FLAG_LOCAL;
        e->holder       = NULL;
        e->holder_shape = NULL;
    } else {
        e->flags        = 0U;
        e->holder       = holder;
        e->holder_shape = holder->shape;
    }
    e->slot_idx     = slot_idx;
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
 * Caches the slot LOCATION (holder, slot_idx) in the IC table keyed by
 * pc_off (refactor-3 GC-06: cache WHERE, not WHAT). */
static struct UClosure *
resolve_op_closure(UVM *vm, UObject *recv_obj,
                   USymbol *op_name, uint32_t pc_off)
{
    /* IC fast path. */
    struct UClosure *cached = ic_lookup(vm, recv_obj, pc_off, op_name);
    if (cached != NULL) {
        return cached;
    }

    /* Slow path: proto-chain walk capturing (holder, idx) so the IC can
     * cache the slot location.  On full-tree miss, retry once with the
     * "fallback" slot — mirrors urbi_object_lookup's GET_FALLBACK
     * retry, which this path used before the GC-06 re-key.  Parity note:
     * urbi_object_resolve_slot bounds the walk at a 64-deep iterative DFS
     * (URBI_RESOLVE_STACK_CAP) where the old urbi_object_lookup recursion
     * was unbounded — same bound the slot-UIC slow path already has. */
    UObject *holder = NULL;
    uint32_t idx    = 0U;
    int rc = urbi_object_resolve_slot(vm, recv_obj, op_name, &holder, &idx);
    if (rc == 0) {
        const USymbol *fb = (const USymbol *)ustr_intern(vm, "fallback", 8);
        if (op_name == fb) {
            return NULL;   /* don't recurse fallback-on-fallback */
        }
        rc = urbi_object_resolve_slot(vm, recv_obj, fb, &holder, &idx);
    }
    /* rc == -1 (resolve-stack overflow) deliberately skips the fallback
     * retry above and lands here: treated as a hard miss. */
    if (rc != 1) {
        return NULL;   /* miss or resolve error */
    }

    UValue v = holder->slots[idx];
    if (v.kind != (uint8_t)UVAL_CLOSURE || v.v.p == NULL) {
        return NULL;   /* found but not a closure */
    }

    /* slot_idx is uint16_t in the entry; indices beyond 65534 are not
     * cacheable (mirrors uic.c's OBJ-IC-POLY saturation) — return the
     * closure uncached so dispatch stays correct. */
    if (idx < 0xFFFFU) {
        ic_fill(vm, recv_obj, pc_off, op_name, holder, (uint16_t)idx);
    }
    return (struct UClosure *)v.v.p;
}

/* -----------------------------------------------------------------------
 * Public helpers: binary and unary fallback
 * --------------------------------------------------------------------- */

/* urbi_vm_arith_method_fallback — binary operator fallback.
 *
 * Called when arith_add/sub/mul/div returns a type error and the lhs is a
 * user object.  Looks up `op_name` slot on lhs's proto chain; if found,
 * calls it with `rhs` as the sole argument and places the result in `*dst`.
 *
 * Returns VM_OP_OVERLOAD_OK if the call succeeded (dst is populated).
 * Returns VM_OP_OVERLOAD_MISS if no usable slot was found (caller should
 * emit the original type error and HALT).
 * Returns VM_OP_OVERLOAD_THREW when the (bytecode) body raised a user
 * exception; *dst holds the thrown value for the caller to re-deposit
 * (refactor-3 VM-07).
 * Returns VM_OP_OVERLOAD_OOM on allocation failure during lookup/call. */
int
urbi_vm_arith_method_fallback(UVM *vm,
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
     * Native path: same as OP_CALL's native-fn arm.  Receiver is lhs;
     * passed directly as native_fn's `self` arg (v1.6 S42 — vm->last_recv
     * is gone).  Args: rhs as args[0].
     *
     * Bytecode path: urbi_run_closure_on_scratch_ex writes rhs into
     * R[0] of the scratch strand.  The function's nparams is typically 1 for
     * binary ops (function(other){...}); the scratch runner does not enforce
     * nparams, so a 0-param body also runs (and ignores the payload). */
    UValue result;
    urbi_zero(&result, sizeof result);
    if (cl->native_fn != NULL) {
        /* GC soundness (v0.13.2 follow-up): root the out-slot for the
         * call's duration, mirroring the main OP_CALL native arm — result
         * is a C stack local, and a native that builds its result
         * incrementally would otherwise hand back a swept cell under
         * collect-on-every-alloc.  The VM-level chain is used (not the
         * strand chain) so the helper stays sound on any future
         * strandless caller.  lhs/rhs point at rooted registers; the arg
         * copy's referent stays alive through them. */
        UCRootFrame f_res;
        UValue arg = *rhs;
        urbi_c_root_push(vm, &f_res, &result);
        int nrc = cl->native_fn(vm, *lhs, &arg, 1, &result);
        urbi_c_root_pop(vm, &f_res);
        if (nrc != 0) {
            return VM_OP_OVERLOAD_MISS;
        }
    } else {
        int threw = 0;
        UExecStatus fatal = UEXEC_OK;
        int src = urbi_run_closure_on_scratch_ex(vm, cl, rhs,
                                                 &result, &threw, &fatal);
        if (src != 0) {
            return VM_OP_OVERLOAD_MISS;
        }
        if (threw) {
            if (fatal == UEXEC_THROW) {
                /* refactor-3 VM-07: the body raised a user exception.  The
                 * scratch runner surfaced the thrown value in `result`;
                 * park it in *dst (the call site's dst register — a GC root
                 * once written) and tell the caller to re-deposit it as a
                 * strand THROW.  No allocation happens between the scratch
                 * runner's copy and this write, so no GC slice can run. */
                *dst = result;
                return VM_OP_OVERLOAD_THREW;
            }
            /* TAG_STOP / CANCEL / budget-exhaustion / yield from an operator
             * body: not a user exception — keep the legacy MISS path
             * (original type error) rather than inventing a cancel-to-throw
             * conversion (refactor-3 VM-07 decision). */
            return VM_OP_OVERLOAD_MISS;
        }
    }

    *dst = result;
    vm_reactive_drain(vm, /*bounded_whenever=*/0);   /* VM-20: drain after operator method call (active level) */
    return VM_OP_OVERLOAD_OK;
}

/* urbi_vm_arith_method_fallback_unary — unary operator fallback.
 *
 * Same as the binary variant but no rhs argument.  Used by OP_NEG.
 * The slot should have nparams=0: `function() { ... }`. */
int
urbi_vm_arith_method_fallback_unary(UVM *vm,
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
    urbi_zero(&result, sizeof result);
    if (cl->native_fn != NULL) {
        /* GC soundness (v0.13.2 follow-up): same out-slot rooting as
         * the binary arms above. */
        UCRootFrame f_res;
        urbi_c_root_push(vm, &f_res, &result);
        int nrc = cl->native_fn(vm, *operand, NULL, 0, &result);
        urbi_c_root_pop(vm, &f_res);
        if (nrc != 0) {
            return VM_OP_OVERLOAD_MISS;
        }
    } else {
        int threw = 0;
        UExecStatus fatal = UEXEC_OK;
        int src = urbi_run_closure_on_scratch_ex(vm, cl, NULL,
                                                 &result, &threw, &fatal);
        if (src != 0) {
            return VM_OP_OVERLOAD_MISS;
        }
        if (threw) {
            if (fatal == UEXEC_THROW) {
                /* refactor-3 VM-07: see urbi_vm_arith_method_fallback — same
                 * thrown-value hand-off through *dst. */
                *dst = result;
                return VM_OP_OVERLOAD_THREW;
            }
            return VM_OP_OVERLOAD_MISS;   /* TAG_STOP/CANCEL/budget: legacy path */
        }
    }

    *dst = result;
    vm_reactive_drain(vm, /*bounded_whenever=*/0);   /* VM-20: drain after operator method call (active level) */
    return VM_OP_OVERLOAD_OK;
}

/* urbi_vm_cmp_method_fallback — comparison operator fallback for OP_EQ / OP_NEQ.
 *
 * Unlike arith ops, uvalue_equal does not raise a type error — it just
 * returns false.  We intercept when lhs is a user object, look up the
 * "==" or "!=" slot, and call it.  The result is coerced to a bool:
 * truthy (non-zero int, non-nil) → true, nil/void/0 → false.
 *
 * Returns VM_OP_OVERLOAD_OK and writes result bool to *out_bool on success.
 * Returns VM_OP_OVERLOAD_MISS if no slot found (caller uses uvalue_equal).
 * Returns VM_OP_OVERLOAD_THREW when the (bytecode) body raised a user
 * exception; *out_thrown holds the thrown value (refactor-3 VM-07). */
int
urbi_vm_cmp_method_fallback(UVM *vm,
                       bool *out_bool,
                       UValue *out_thrown,
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
    urbi_zero(&result, sizeof result);
    if (cl->native_fn != NULL) {
        /* GC soundness (v0.13.2 follow-up): root the out-slot for the
         * call's duration, mirroring the main OP_CALL native arm — result
         * is a C stack local, and a native that builds its result
         * incrementally would otherwise hand back a swept cell under
         * collect-on-every-alloc.  The VM-level chain is used (not the
         * strand chain) so the helper stays sound on any future
         * strandless caller.  lhs/rhs point at rooted registers; the arg
         * copy's referent stays alive through them. */
        UCRootFrame f_res;
        UValue arg = *rhs;
        urbi_c_root_push(vm, &f_res, &result);
        int nrc = cl->native_fn(vm, *lhs, &arg, 1, &result);
        urbi_c_root_pop(vm, &f_res);
        if (nrc != 0) {
            return VM_OP_OVERLOAD_MISS;
        }
    } else {
        int threw = 0;
        UExecStatus fatal = UEXEC_OK;
        int src = urbi_run_closure_on_scratch_ex(vm, cl, rhs,
                                                 &result, &threw, &fatal);
        if (src != 0) {
            return VM_OP_OVERLOAD_MISS;
        }
        if (threw) {
            if (fatal == UEXEC_THROW) {
                /* refactor-3 VM-07: comparisons have no dst register, so the
                 * thrown value is handed off via *out_thrown.  The caller
                 * must re-deposit it into s->unwind_value (a rooted strand
                 * field) before any allocation can run a GC slice; the
                 * dispatch arms do this immediately on THREW. */
                *out_thrown = result;
                return VM_OP_OVERLOAD_THREW;
            }
            return VM_OP_OVERLOAD_MISS;   /* TAG_STOP/CANCEL/budget: legacy path */
        }
    }

    /* Coerce result to bool using the single canonical truthiness predicate.
     * Delegates to uvalue_truthy — one truth source. */
    *out_bool = uvalue_truthy(&result);

    vm_reactive_drain(vm, /*bounded_whenever=*/0);   /* VM-20: drain after operator method call (active level) */
    return VM_OP_OVERLOAD_OK;
}
