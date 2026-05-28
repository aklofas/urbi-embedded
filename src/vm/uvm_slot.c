/* SPDX-License-Identifier: BSD-3-Clause */
/* src/vm/uvm_slot.c — slot-access helper implementations.
 * See uvm_slot.h for the function contracts. */

#include "vm/uvm_slot.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vm/uvm_internal.h"          /* UDiagWriter, diag_*, vm_format_* */
#include "object/uic.h"               /* UIC, URBI_IC_ENTRIES_PER_SITE */
#include "object/ushape.h"            /* UProps */
#include "object/uobject.h"           /* UObject, URBI_SLOT_FLAG_* */
#include "watcher/uwatcher.h"         /* URBI_WATCHER_READSET_MAX */
#include "watcher/uwatcher_install.h" /* urbi_run_closure_on_scratch */
#include "changed/uchanged_node.h"    /* urbi_emit_slot_change_if_subscribed */
#include "urbi/gc.h"                  /* urbi_gc_slot_store, urbi_gc_slot_pre_store */
#include "gc/ugc.h"                   /* UCell */

/* -----------------------------------------------------------------------
 * vm_trace_slot_read_if_needed
 * ----------------------------------------------------------------------- */

void
vm_trace_slot_read_if_needed(UVM *vm, UObject *recv)
{
    /* Pre: urbi_vm_init succeeded, so vm->watchers is non-NULL. */
    if (!vm->watchers->in_install)
        return;

    UCell *cell = (UCell *)recv;
    size_t i;
    for (i = 0; i < (size_t)vm->trace_read_set_count; i++) {
        if (vm->trace_read_set[i] == cell)
            return; /* already present */
    }
    if ((size_t)vm->trace_read_set_count < (size_t)URBI_WATCHER_READSET_MAX) {
        vm->trace_read_set[vm->trace_read_set_count++] = cell;
    } else {
        vm->trace_overflow = 1;
    }
}

/* -----------------------------------------------------------------------
 * vm_resolve_ic
 *
 * The LOCAL-slot re-dispatch discipline lives exclusively here (OBJ-IC-POLY
 * fix, audit-1 F3 + runtime-invariants F8).  After W1 the three OP arms
 * that previously each inlined this logic call through here instead.
 * ----------------------------------------------------------------------- */

UVmSlotResult
vm_resolve_ic(UVM *vm,
              UIC *ic,
              UObject *recv,
              bool writing,
              UValue v_write,
              UValue *out_value,
              uint8_t *out_fresh_k)
{
    for (uint8_t k = 0; k < ic->n; k++) {
        if (ic->recv_shapes[k]  != recv->shape)   continue;
        if (ic->topology_gen[k] != vm->topology_gen) continue;

        /* IC fast-path hit (shape guard matched). */
        URBI_PERF_INC(vm, ic_hit);
        if (!writing) {
            /* Get path. */
            if (ic->flags[k] & URBI_SLOT_FLAG_OGET) {
                if (out_fresh_k) *out_fresh_k = k;
                return VM_SLOT_GETTER_NEEDED;
            }
            /* OBJ-IC-POLY: re-resolve local slot per receiver.
             * The IC caches a slot index (not an absolute pointer) for
             * FLAG_LOCAL entries so the same IC entry serves all same-shape
             * receivers correctly. */
            UValue loaded = (ic->flags[k] & URBI_SLOT_FLAG_LOCAL)
                            ? recv->slots[ic->slot_idx[k]]
                            : *ic->slots[k];
            if (out_value) *out_value = loaded;
            return VM_SLOT_OK;
        } else {
            /* Set path. */
            if (ic->flags[k] & URBI_SLOT_FLAG_OSET) {
                if (out_fresh_k) *out_fresh_k = k;
                return VM_SLOT_SETTER_NEEDED;
            }
            if (ic->flags[k] & URBI_SLOT_FLAG_CONSTANT) {
                return VM_SLOT_CONST_WRITE;
            }
            if (ic->flags[k] & URBI_SLOT_FLAG_LOCAL) {
                /* OBJ-IC-POLY: write to the per-receiver slot using the
                 * cached index, NOT to ic->slots[k] (which is recv-specific
                 * for the first instance that filled the IC entry). */
                uint32_t s_idx = (uint32_t)ic->slot_idx[k];
                urbi_gc_slot_store(vm, (UCell *)recv, s_idx,
                                   &recv->slots[s_idx], v_write);
                urbi_emit_slot_change_if_subscribed(vm, recv, ic->name, v_write);
                return VM_SLOT_OK;
            }
            /* Proto-chain hit (no LOCAL, no OSET, no CONSTANT): the slot
             * lives on a parent; fall to slow path for COW. */
            return VM_SLOT_MISSING;
        }
    }

    /* No matching IC entry — slow path needed. */
    URBI_PERF_INC(vm, ic_miss);
    return VM_SLOT_MISSING;
}

/* -----------------------------------------------------------------------
 * vm_dispatch_getter
 * ----------------------------------------------------------------------- */

UVmSlotResult
vm_dispatch_getter(UVM *vm,
                   UProps *up,
                   const char *opname,
                   UValue *out_result)
{
    if (up == NULL || up->oget.kind != (uint8_t)UVAL_CLOSURE
                   || up->oget.v.p == NULL) {
        vm->last_error = UVM_TYPE_ERROR;
        {
            UDiagWriter _w;
            diag_init(&_w, vm->last_errmsg, UVM_ERRMSG_CAP);
            diag_write_cstr(&_w, "TypeError: ");
            diag_write_cstr(&_w, opname);
            diag_write_cstr(&_w, ": getter is not a closure");
        }
        return VM_SLOT_MISSING;
    }
    UValue result; int threw = 0;
    int rc = urbi_run_closure_on_scratch(vm, (UClosure *)up->oget.v.p,
                                         &result, &threw);
    if (rc != 0 || threw) {
        vm->last_error = UVM_TYPE_ERROR;
        vm_format_type_error_msg(vm, "getter raised");
        return VM_SLOT_MISSING;
    }
    if (out_result) *out_result = result;
    return VM_SLOT_OK;
}

/* -----------------------------------------------------------------------
 * vm_dispatch_setter
 * ----------------------------------------------------------------------- */

UVmSlotResult
vm_dispatch_setter(UVM *vm,
                   UProps *up,
                   const char *opname,
                   UValue payload)
{
    if (up == NULL || up->oset.kind != (uint8_t)UVAL_CLOSURE
                   || up->oset.v.p == NULL) {
        vm->last_error = UVM_TYPE_ERROR;
        {
            UDiagWriter _w;
            diag_init(&_w, vm->last_errmsg, UVM_ERRMSG_CAP);
            diag_write_cstr(&_w, "TypeError: ");
            diag_write_cstr(&_w, opname);
            diag_write_cstr(&_w, ": setter is not a closure");
        }
        return VM_SLOT_MISSING;
    }
    UValue result; int threw = 0;
    int rc = urbi_run_closure_on_scratch_with_payload(
                vm, (UClosure *)up->oset.v.p, payload, &result, &threw);
    if (rc != 0 || threw) {
        vm->last_error = UVM_TYPE_ERROR;
        vm_format_type_error_msg(vm, "setter raised");
        return VM_SLOT_MISSING;
    }
    /* Setter return value is discarded. */
    return VM_SLOT_OK;
}

/* -----------------------------------------------------------------------
 * vm_getslot_value
 * ----------------------------------------------------------------------- */

UVmSlotResult
vm_getslot_value(UVM *vm,
                 UIC *ic,
                 UObject *recv,
                 UValue *out_value,
                 uint8_t *out_fresh_k)
{
    vm_trace_slot_read_if_needed(vm, recv);
    return vm_resolve_ic(vm, ic, recv, /*writing=*/false,
                         /*v_write=*/(UValue){0},
                         out_value, out_fresh_k);
}

/* -----------------------------------------------------------------------
 * vm_setslot_value
 * ----------------------------------------------------------------------- */

UVmSlotResult
vm_setslot_value(UVM *vm,
                 UIC *ic,
                 UObject *recv,
                 UValue v,
                 uint8_t *out_fresh_k)
{
    return vm_resolve_ic(vm, ic, recv, /*writing=*/true, v,
                         /*out_value=*/NULL, out_fresh_k);
}

/* -----------------------------------------------------------------------
 * vm_getslot_slow
 * ----------------------------------------------------------------------- */

UVmSlotResult
vm_getslot_slow(UVM *vm,
                UIC *ic,
                UObject *recv,
                const char *opname,
                UValue *out_value)
{
    UValue v;
    int rc = urbi_slot_get_slow(vm, recv, ic, &v);
    if (rc != 0) {
        vm->last_error = UVM_TYPE_ERROR;
        {
            UDiagWriter _w;
            diag_init(&_w, vm->last_errmsg, UVM_ERRMSG_CAP);
            diag_write_cstr(&_w, "TypeError: ");
            diag_write_cstr(&_w, opname);
            diag_write_cstr(&_w, ": slot '");
            if (ic->name != NULL)
                diag_write_cstr(&_w, (const char *)ic->name);
            diag_write_cstr(&_w, "' not found");
        }
        return VM_SLOT_MISSING;
    }
    /* Inspect the freshly-filled IC entry for a pending getter. */
    uint8_t fresh_k = (uint8_t)((ic->replace_cursor + URBI_IC_ENTRIES_PER_SITE - 1U)
                                % URBI_IC_ENTRIES_PER_SITE);
    if (ic->n > 0U && (ic->flags[fresh_k] & URBI_SLOT_FLAG_OGET)) {
        UVmSlotResult gr = vm_dispatch_getter(vm, ic->uprops[fresh_k], opname, out_value);
        return gr; /* VM_SLOT_OK (getter result in *out_value) or error */
    }
    if (out_value) *out_value = v;
    return VM_SLOT_OK;
}

/* -----------------------------------------------------------------------
 * vm_setslot_slow
 * ----------------------------------------------------------------------- */

UVmSlotResult
vm_setslot_slow(UVM *vm,
                UIC *ic,
                UObject *recv,
                UValue v,
                const char *opname)
{
    /* Snapshot recv's shape before the slow path: if the shape changes after
     * the call, the slow path installed a new slot (first-time or COW).  Per
     * the legacy semantic, slot-change must NOT fire on install — only on
     * writes to an already-existing slot.  Compare shape pointers post-call
     * to suppress the emit on the install path. */
    const UShape *shape_before = recv->shape;

    int rc = urbi_slot_set_slow(vm, recv, ic, v);
    if (rc != 0) {
        vm->last_error = UVM_TYPE_ERROR;
        {
            UDiagWriter _w;
            diag_init(&_w, vm->last_errmsg, UVM_ERRMSG_CAP);
            diag_write_cstr(&_w, "TypeError: ");
            diag_write_cstr(&_w, opname);
            diag_write_cstr(&_w, ": slot write failed for '");
            if (ic->name != NULL)
                diag_write_cstr(&_w, (const char *)ic->name);
            diag_write_cstr(&_w, "' (constant, OOM, or resolve overflow)");
        }
        return VM_SLOT_MISSING;
    }
    /* Check if the freshly-filled IC entry has a pending setter. */
    uint8_t fresh_k = (uint8_t)((ic->replace_cursor + URBI_IC_ENTRIES_PER_SITE - 1U)
                                % URBI_IC_ENTRIES_PER_SITE);
    if (ic->n > 0U && (ic->flags[fresh_k] & URBI_SLOT_FLAG_OSET)) {
        return vm_dispatch_setter(vm, ic->uprops[fresh_k], opname, v);
    }
    /* Barrier-only post-slow-path: the store is done inside urbi_slot_set_slow;
     * fire observer_dirty so watchers whose read-set includes recv see the write.
     * Slot index 0 is a conservative sentinel — observer_dirty ignores the key. */
    urbi_gc_slot_pre_store(vm, (UCell *)recv, 0U, v);
    /* Suppress the slot-change emit when the slow path installed a new slot
     * (shape transitioned = first-time install or COW).  Install is not a
     * change per the legacy semantic; closes v0.10.7-C. */
    if (recv->shape == shape_before) {
        urbi_emit_slot_change_if_subscribed(vm, recv, ic->name, v);
    }
    return VM_SLOT_OK;
}
