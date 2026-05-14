/* SPDX-License-Identifier: BSD-3-Clause */
/* uvm_ref.c — Per-VM reference table: urbi_ref, urbi_ref_get, urbi_unref
 * (Gap Q, v0.7.1).
 *
 * Design:
 *   The reference table is a growable heap-allocated array of URefSlot
 *   entries.  Each slot holds the pinned UValue, a generation counter,
 *   and an in_use flag.  Slot 0 is permanently reserved so that the
 *   sentinel URBI_REF_INVALID (== 0) can never be a valid handle.
 *
 *   Handle encoding: (slot_index << 8) | generation.
 *     - URBI_REF_INVALID == 0 → slot 0, gen 0 → permanently free slot 0.
 *     - Valid handles have (handle >> 8) >= 1.
 *     - urbi_unref increments generation mod 256 → old handle becomes stale.
 *
 *   Free-list: a singly-linked list threaded through the pad[0] byte (as
 *   an index into slots[]) when the slot is free.  free_list_head == SIZE_MAX
 *   means "no free slots; grow the table".
 *
 *   GC integration: ref_table_walk_roots is registered at urbi_vm_init.
 *   It calls the GC root callback for every in_use slot value, keeping
 *   pinned values alive across collection cycles.
 *
 * Freestanding discipline: no <string.h>.  Byte-zero via urbi_zero. */

#include "vm/uvm.h"
#include "vm/uvm_error.h"   /* urbi_set_error_internal */
#include "vm/uvm_ref.h"     /* ref_table_walk_roots declaration */
#include "runtime/umacros.h" /* urbi_zero */
#include "urbi/urbi.h"       /* urbi_ref, urbi_ref_get, urbi_unref, URBI_REF_INVALID */
#include "urbi/types.h"      /* UValue, urbi_make_nil, URBI_ERR_OOM */
#include "gc/ugc.h"          /* UGcRootCallback */

#include <stddef.h>
#include <stdint.h>

/* Initial capacity for the slots array.  Includes the reserved slot 0
 * sentinel, so the first user-visible slot is index 1. */
#define REF_TABLE_INIT_CAP  8U

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* Encode a (slot_index, generation) pair into a handle.
 * Precondition: slot_index >= 1 (slot 0 is the sentinel). */
static urbi_ref_t
encode_handle(size_t slot_index, uint8_t generation)
{
    return (urbi_ref_t)((slot_index << 8U) | (uint32_t)generation);
}

/* Decode slot index from handle (upper 24 bits). */
static size_t
handle_index(urbi_ref_t ref)
{
    return (size_t)(ref >> 8U);
}

/* Decode generation from handle (lower 8 bits). */
static uint8_t
handle_gen(urbi_ref_t ref)
{
    return (uint8_t)(ref & 0xFFU);
}

/* Grow the slots array.  Doubles capacity.  Returns 0 on success, -1 on OOM
 * or if the VM has no allocator (freestanding; caller must pre-allocate). */
static int
ref_table_grow(UVM *vm)
{
    size_t old_cap = vm->ref_table.capacity;
    size_t new_cap;
    URefSlot *grown;
    size_t i;

    if (old_cap == 0U) {
        new_cap = REF_TABLE_INIT_CAP;
    } else {
        /* Double; guard against overflow. */
        if (old_cap >= (size_t)(URBI_REF_MAX_SLOTS >> 1U)) {
            /* Would overflow 24-bit index space. */
            return -1;
        }
        new_cap = old_cap * 2U;
    }
    if (new_cap > (size_t)URBI_REF_MAX_SLOTS) {
        new_cap = (size_t)URBI_REF_MAX_SLOTS;
    }
    if (new_cap <= old_cap) return -1;  /* already at max */

    if (vm->alloc_fn == NULL) return -1;

    grown = (URefSlot *)vm->alloc_fn(vm->ref_table.slots,
                                     new_cap * sizeof(URefSlot),
                                     vm->alloc_ud);
    if (grown == NULL) return -1;

    /* Zero-init the newly added slots.  Slot 0 was already zeroed on
     * first allocation; subsequent grows only zero the new tail. */
    urbi_zero(&grown[old_cap],
              (new_cap - old_cap) * sizeof(URefSlot));

    /* Wire new slots into the free list (in reverse so lower indices are
     * preferred — improves locality). */
    for (i = new_cap - 1U; i >= old_cap && i >= 1U; i--) {
        /* Store next free-list index in pad[0..3].  We repurpose pad[0]
         * as the low byte and (if SIZE_MAX, flag with 0xFF sentinel). */
        if (vm->ref_table.free_list_head == (size_t)-1) {
            /* Terminator: store SIZE_MAX sentinel in pad bytes. */
            grown[i].pad[0] = 0xFFU;
            grown[i].pad[1] = 0xFFU;
            grown[i].pad[2] = 0xFFU;
        } else {
            size_t next = vm->ref_table.free_list_head;
            /* Pack next into pad[0..2] (24-bit index fits). */
            grown[i].pad[0] = (uint8_t)(next & 0xFFU);
            grown[i].pad[1] = (uint8_t)((next >> 8U) & 0xFFU);
            grown[i].pad[2] = (uint8_t)((next >> 16U) & 0xFFU);
        }
        vm->ref_table.free_list_head = i;
    }

    /* Slot 0 stays permanently in_use=0 and generation=0; never enters
     * the free list (URBI_REF_INVALID == handle(0, 0) must stay invalid). */

    vm->ref_table.slots    = grown;
    vm->ref_table.capacity = new_cap;
    return 0;
}

/* Read the free-list next-index from a slot's pad bytes. */
static size_t
slot_free_list_next(const URefSlot *s)
{
    if (s->pad[0] == 0xFFU && s->pad[1] == 0xFFU && s->pad[2] == 0xFFU) {
        return (size_t)-1;  /* SIZE_MAX sentinel */
    }
    return (size_t)s->pad[0]
         | ((size_t)s->pad[1] << 8U)
         | ((size_t)s->pad[2] << 16U);
}

/* Write the free-list next-index into a slot's pad bytes. */
static void
slot_free_list_set_next(URefSlot *s, size_t next_idx)
{
    if (next_idx == (size_t)-1) {
        s->pad[0] = 0xFFU;
        s->pad[1] = 0xFFU;
        s->pad[2] = 0xFFU;
    } else {
        s->pad[0] = (uint8_t)(next_idx & 0xFFU);
        s->pad[1] = (uint8_t)((next_idx >> 8U) & 0xFFU);
        s->pad[2] = (uint8_t)((next_idx >> 16U) & 0xFFU);
    }
}

/* =========================================================================
 * GC root provider
 * ========================================================================= */

/* ref_table_walk_roots: registered at urbi_vm_init.
 *
 * Called by the GC mark phase to enumerate all pinned values as roots.
 * For every in_use slot, calls cb(vm, &slot.value, ctx) so the mark-
 * root path shades the value's cell gray.
 *
 * Slot 0 is always in_use == 0 (sentinel) so it is never visited. */
void
ref_table_walk_roots(struct UVM *vm, UGcRootCallback cb, void *ctx)
{
    size_t i;
    if (vm->ref_table.slots == NULL) return;
    for (i = 1U; i < vm->ref_table.capacity; i++) {
        URefSlot *s = &vm->ref_table.slots[i];
        if (s->in_use) {
            cb(vm, &s->value, ctx);
        }
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/* urbi_ref: pin a UValue as a GC root.
 *
 * Allocates a free slot (growing the table if needed), stores value,
 * marks in_use, and returns a handle encoding (slot_index << 8) | generation.
 * Returns URBI_REF_INVALID on OOM or if the table is at max capacity. */
urbi_ref_t
urbi_ref(struct UVM *vm, UValue value)
{
    size_t idx;
    URefSlot *s;
    urbi_ref_t handle;

    if (vm == NULL) return URBI_REF_INVALID;

    /* Ensure there is a free slot. */
    if (vm->ref_table.free_list_head == (size_t)-1) {
        /* No free slots — try to grow. */
        if (ref_table_grow(vm) != 0) {
            urbi_set_error_internal(vm, URBI_ERR_OOM,
                "urbi_ref: ref table full or OOM",
                NULL, 0, "urbi_ref");
            return URBI_REF_INVALID;
        }
    }

    /* Pop from free list. */
    idx = vm->ref_table.free_list_head;
    s   = &vm->ref_table.slots[idx];
    vm->ref_table.free_list_head = slot_free_list_next(s);

    /* Zero the pad bytes before marking in_use (free-list fields). */
    s->pad[0] = 0U;
    s->pad[1] = 0U;
    s->pad[2] = 0U;

    /* Install the value. */
    s->value  = value;
    s->in_use = 1U;
    /* generation stays as-is (was incremented by last urbi_unref on this slot). */

    vm->ref_table.used++;

    handle = encode_handle(idx, s->generation);
    return handle;
}

/* urbi_ref_get: retrieve the pinned UValue for a live handle.
 *
 * Returns urbi_make_nil() if the handle is invalid, stale, or vm == NULL. */
UValue
urbi_ref_get(struct UVM *vm, urbi_ref_t ref)
{
    size_t idx;
    uint8_t gen;
    const URefSlot *s;

    if (vm == NULL || ref == URBI_REF_INVALID) return urbi_make_nil();

    idx = handle_index(ref);
    gen = handle_gen(ref);

    if (idx == 0U || idx >= vm->ref_table.capacity) return urbi_make_nil();

    s = &vm->ref_table.slots[idx];
    if (!s->in_use || s->generation != gen) return urbi_make_nil();

    return s->value;
}

/* urbi_unref: release a pinned handle.
 *
 * Increments the slot's generation counter (invalidating old handle copies)
 * and returns the slot to the free list.  No-op on invalid/stale handles. */
void
urbi_unref(struct UVM *vm, urbi_ref_t ref)
{
    size_t idx;
    uint8_t gen;
    URefSlot *s;

    if (vm == NULL || ref == URBI_REF_INVALID) return;

    idx = handle_index(ref);
    gen = handle_gen(ref);

    if (idx == 0U || idx >= vm->ref_table.capacity) return;

    s = &vm->ref_table.slots[idx];
    if (!s->in_use || s->generation != gen) return;  /* stale handle */

    /* Clear the value, mark free. */
    s->value  = urbi_make_nil();
    s->in_use = 0U;
    s->generation = (uint8_t)((uint8_t)(s->generation + 1U));  /* wraps mod 256 */

    /* Prepend to free list. */
    slot_free_list_set_next(s, vm->ref_table.free_list_head);
    vm->ref_table.free_list_head = idx;

    if (vm->ref_table.used > 0U) {
        vm->ref_table.used--;
    }
}
