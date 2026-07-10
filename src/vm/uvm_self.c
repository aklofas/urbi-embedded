/* SPDX-License-Identifier: BSD-3-Clause */
/* src/vm/uvm_self.c — OP_SELF helper.
 *
 * Extracted from src/vm/uvm.c OP_SELF arm (lines 1821-1953).
 * Built on urbi_vm_getslot_value + receiver copy.
 *
 * urbi_vm_self_lookup: OP_SELF semantics — same slot lookup as OP_GETSLOT,
 * PLUS a guaranteed receiver-register write at *out_recv.
 *
 * The receiver-preservation contract: *out_recv holds recv_value on
 * return regardless of slot-find result.  This is set FIRST so that
 * subsequent slow-path branches and HALT() paths in the caller still
 * have a usable receiver for OP_CALL diagnostic formatting.
 *
 * dst_reg may alias recv_reg; OP_SELF must write R[A+1] = self_value
 * BEFORE R[A] = method_value to avoid destroying the receiver snapshot
 * (captured by the caller before calling us).  The caller does both
 * register writes after urbi_vm_self_lookup returns. */

#include "vm/uvm_slot.h"

#include "vm/uvm.h"
#include "object/uobject.h"

/* urbi_vm_self_lookup: look up a slot for OP_SELF.
 *
 * vm      — VM context
 * ic      — IC entry table for this call site
 * recv    — resolved UObject* (after atom-proto substitution)
 * out_slot  — receives loaded slot value on UVM_SLOT_OK, or getter result
 *             via urbi_vm_dispatch_getter when UVM_SLOT_GETTER_NEEDED would
 *             have been returned (the OP_SELF arm dispatches getters here
 *             rather than in uvm.c to keep the arm thin)
 * out_fresh_k — receives the IC entry index on UVM_SLOT_GETTER_NEEDED;
 *              unused for UVM_SLOT_OK
 *
 * Return value: same semantics as urbi_vm_getslot_value.
 * UVM_SLOT_GETTER_NEEDED means the caller should call urbi_vm_dispatch_getter
 * (OP_SELF handles getters identically to OP_GETSLOT, including the
 * additional R[A+1] = self_value write). */
UVmSlotResult
urbi_vm_self_lookup(UVM *vm,
               UIC *ic,
               UObject *recv,
               UValue *out_slot,
               uint8_t *out_fresh_k)
{
    /* urbi_vm_getslot_value calls vm_trace_slot_read_if_needed internally
     * (same watcher-install trace semantics as OP_GETSLOT). */
    return urbi_vm_getslot_value(vm, ic, recv, out_slot, out_fresh_k);
}
