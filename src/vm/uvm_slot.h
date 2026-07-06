/* SPDX-License-Identifier: BSD-3-Clause */
/* src/vm/uvm_slot.h — slot-access helpers extracted from the OP_GETSLOT /
 * OP_SETSLOT / OP_SELF arms of the VM dispatch loop (Wave 5, W1).
 *
 * These helpers collapse the LOCAL-slot re-dispatch discipline to a
 * single site (vm_resolve_ic) and the read-set trace probe to a single site
 * (vm_trace_slot_read_if_needed — both static in uvm_slot.c), closing
 * audit findings:
 *   audit-1 F3   — LOCAL-slot re-dispatch duplicated in 3 OP arms
 *   runtime-invariants F8 — inline FLAG_LOCAL re-resolution spread
 *   bytecode F4  — (partial) IC discipline not factored
 *
 * Consumed only by uvm.c, uvm_slot.c, and uvm_self.c.
 * NOT part of the public API; no versioning obligation. */

#ifndef UVM_SLOT_H
#define UVM_SLOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vm/uvm.h"
#include "vm/uvm_internal.h"
#include "object/uic.h"
#include "object/uobject.h"
#include "sched/ustrand.h"

/* UVmSlotResult — return codes from urbi_vm_getslot_value and urbi_vm_setslot_value.
 *
 * OK           — value loaded / stored successfully into *out (get) or recv.
 * GETTER_NEEDED — IC entry has FLAG_OGET; caller must invoke getter closure.
 * SETTER_NEEDED — IC entry has FLAG_OSET; caller must invoke setter closure.
 * MISSING       — slow-path slot_get returned "not found".
 * CONST_WRITE   — attempted write to a FLAG_CONSTANT slot.
 * THREW         — helper deposited a catchable typed throw on vm->cur_strand;
 *                 caller must `goto safepoint`.
 */
typedef enum {
    UVM_SLOT_OK           = 0,
    UVM_SLOT_GETTER_NEEDED,
    UVM_SLOT_SETTER_NEEDED,
    UVM_SLOT_MISSING,
    UVM_SLOT_CONST_WRITE,
    UVM_SLOT_THREW
} UVmSlotResult;

/* urbi_vm_getslot_value: IC-fast-path get for OP_GETSLOT.
 *
 * Combines vm_trace_slot_read_if_needed + vm_resolve_ic (read path).
 * On UVM_SLOT_OK, *out_value holds the loaded value.
 * On UVM_SLOT_GETTER_NEEDED, the IC entry index is in *out_fresh_k and
 *   ic->uprops[*out_fresh_k].oget holds the getter closure.
 * On UVM_SLOT_MISSING, the caller must take the slow path. */
UVmSlotResult urbi_vm_getslot_value(UVM *vm,
                                UIC *ic,
                                UObject *recv,
                                UValue *out_value,
                                uint8_t *out_fresh_k);

/* urbi_vm_dispatch_getter: invoke getter closure on a transient scratch strand.
 *
 * `up` must be ic->uprops[k] where flags[k] has FLAG_OGET set.
 * `opname` is the opcode name string used in diagnostic messages.
 * On success, *out_result holds the return value.
 * Returns UVM_SLOT_OK on success; UVM_SLOT_MISSING on error
 * (vm->last_error set; caller should HALT). */
UVmSlotResult urbi_vm_dispatch_getter(UVM *vm,
                                  UProps *up,
                                  const char *opname,
                                  UValue *out_result);

/* urbi_vm_setslot_value: IC-fast-path set for OP_SETSLOT.
 *
 * Handles FLAG_LOCAL (write + GC barrier + slot-change), FLAG_OSET (returns
 * UVM_SLOT_SETTER_NEEDED), FLAG_CONSTANT (returns UVM_SLOT_CONST_WRITE), and
 * proto-chain hit (returns UVM_SLOT_MISSING so caller falls to slow path).
 * Returns UVM_SLOT_OK when the fast-path local write succeeded. */
UVmSlotResult urbi_vm_setslot_value(UVM *vm,
                                UIC *ic,
                                UObject *recv,
                                UValue v,
                                uint8_t *out_fresh_k);

/* urbi_vm_dispatch_setter: invoke setter closure on a transient scratch strand
 * with `payload` as the argument.
 *
 * `up` must be ic->uprops[k] where flags[k] has FLAG_OSET set.
 * Setter return value is discarded (OP_SETSLOT has no scripted result).
 * Returns UVM_SLOT_OK on success; UVM_SLOT_MISSING on error
 * (vm->last_error set; caller should HALT). */
UVmSlotResult urbi_vm_dispatch_setter(UVM *vm,
                                  UProps *up,
                                  const char *opname,
                                  UValue payload);

/* urbi_vm_self_lookup: OP_SELF semantics — same as urbi_vm_getslot_value but
 * living in uvm_self.c to keep the self-lookup rationale co-located
 * with the receiver-preservation contract.
 *
 * Returns the same result codes as urbi_vm_getslot_value. */
UVmSlotResult urbi_vm_self_lookup(UVM *vm,
                              UIC *ic,
                              UObject *recv,
                              UValue *out_slot,
                              uint8_t *out_fresh_k);

/* urbi_vm_getslot_slow: slow-path slot get for OP_GETSLOT and OP_SELF.
 *
 * Calls urbi_slot_get_slow, formats "not found" error on failure,
 * checks the freshly-filled IC entry for FLAG_OGET and dispatches the
 * getter closure inline if present.
 *
 * Returns UVM_SLOT_OK with *out_value filled on success (including after
 * a getter dispatch — the getter result lands in *out_value).
 * Returns UVM_SLOT_MISSING on error (slot not found or getter raised;
 * vm->last_error set in both cases).
 *
 * `opname` is used in error messages ("slot access" or "method call"). */
UVmSlotResult urbi_vm_getslot_slow(UVM *vm,
                               UIC *ic,
                               UObject *recv,
                               const char *opname,
                               UValue *out_value);

/* urbi_vm_setslot_slow: slow-path slot set for OP_SETSLOT.
 *
 * Calls urbi_slot_set_slow, formats error on failure, dispatches setter
 * if the freshly-filled entry has FLAG_OSET, fires the GC barrier +
 * slot-change event after a plain store.
 *
 * Returns UVM_SLOT_OK on success.
 * Returns UVM_SLOT_MISSING on failure (vm->last_error set).
 * Returns UVM_SLOT_THREW when the setter raised a typed error
 *   (vm->last_error set; caller must `goto safepoint`).
 *
 * `opname` is used in error messages ("slot write"). */
UVmSlotResult urbi_vm_setslot_slow(UVM *vm,
                               UIC *ic,
                               UObject *recv,
                               UValue v,
                               const char *opname);

#endif /* UVM_SLOT_H */
