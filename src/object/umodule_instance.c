/* SPDX-License-Identifier: BSD-3-Clause */
/* umodule_instance.c — UModuleInstance + UProtoInstanceArr lifecycle.
 *
 * See umodule_instance.h for the design contract and layout invariants. */

#include "object/umodule_instance.h"

#include "urbi/gc.h"          /* urbi_gc_alloc, UTYPE_MODULE_INSTANCE, UTYPE_PROTO_INSTANCE */
#include "vm/uvm.h"              /* UVM (for the typed pointer) */

#include <stddef.h>
#include <stdint.h>
#include "gc/ugc.h"
#include "module/umodule.h"
#include "object/uic.h"

/* init_ic_slice — zero-fill an IC table slice carved from ic_cursor.
 * Fills pi->ic_table with ic_count entries sourced from ic_names (may be
 * NULL), advances *cursor by ic_count, and sets pi->proto = proto.
 * When ic_count == 0, pi->ic_table is set to NULL and *cursor is unchanged. */
static void
init_ic_slice(UProtoInstance *pi, UProto *proto,
              uint16_t ic_count, USymbol **ic_names,
              UIC **cursor)
{
    pi->proto = proto;
    if (ic_count == 0U) {
        pi->ic_table = NULL;
        return;
    }
    pi->ic_table = *cursor;
    for (uint16_t k = 0U; k < ic_count; k++) {
        UIC *ic = &(*cursor)[k];
        ic->name           = (ic_names != NULL) ? ic_names[k] : NULL;
        ic->n              = 0U;
        ic->replace_cursor = 0U;
        for (int e = 0; e < URBI_IC_ENTRIES_PER_SITE; e++) {
            ic->recv_shapes[e]  = NULL;
            ic->topology_gen[e] = 0U;
            ic->slots[e]        = NULL;
            ic->uprops[e]       = NULL;
            ic->flags[e]        = 0U;
        }
    }
    *cursor += ic_count;
}

UModuleInstance *
urbi_module_instance_create(struct UVM *vm, UModule *m)
{
    if (vm == NULL || m == NULL) {
        return NULL;
    }

    /* Cell 1: UModuleInstance.  Cast cell pointer to struct (UCell is the
     * first member; addresses coincide).  Caller is responsible for OOM. */
    UCell *mi_cell = urbi_gc_alloc(vm, sizeof(UModuleInstance),
                                   UTYPE_MODULE_INSTANCE);
    if (mi_cell == NULL) {
        return NULL;
    }
    UModuleInstance *mi = (UModuleInstance *)mi_cell;
    mi->module          = m;
    mi->vm              = vm;
    mi->proto_instances = NULL;   /* publish only after the second cell is wired */
    mi->next_in_vm      = NULL;   /* T30: thread onto vm->module_instances_head below */

    /* Cell 2: UProtoInstanceArr bulk.  Layout = [header pad] + entries[n] +
     * IC tables for root chunk + every nested proto's ic_count.
     * entries[0] is the root chunk; entries[1..n-1] mirror module->nested[].
     *
     * Bulk size = sizeof(UProtoInstanceArr)              (header + cell + pad)
     *           + n * sizeof(UProtoInstance)              (entries[] payload)
     *           + m->ic_count * sizeof(UIC)              (root-chunk IC region)
     *           + sum(nested[i]->ic_count) * sizeof(UIC)  (nested IC region) */
    uint16_t n = (uint16_t)(1U + m->nested_count);

    size_t entries_bytes = (size_t)n * sizeof(UProtoInstance);

    size_t ic_bytes = (size_t)m->ic_count * sizeof(UIC);  /* root-chunk ICs */
    for (size_t i = 0U; i < m->nested_count; i++) {
        UProto *p = m->nested[i];
        if (p == NULL) continue;
        ic_bytes += (size_t)p->ic_count * sizeof(UIC);
    }

    size_t arr_size = sizeof(UProtoInstanceArr) + entries_bytes + ic_bytes;

    UCell *arr_cell = urbi_gc_alloc(vm, arr_size, UTYPE_PROTO_INSTANCE);
    if (arr_cell == NULL) {
        /* mi is GC-managed; sweep will reap it.  No partial state to undo. */
        return NULL;
    }
    UProtoInstanceArr *arr = (UProtoInstanceArr *)arr_cell;
    arr->n        = n;
    arr->_pad[0]  = 0U;
    arr->_pad[1]  = 0U;
    arr->_pad[2]  = 0U;

    /* IC tables live immediately after entries[].  Compute base by
     * pointer-arithmetic past the FAM. */
    UIC *ic_cursor = (UIC *)(void *)((uint8_t *)arr->entries + entries_bytes);

    /* entries[0]: root chunk.  ic_table populated from UModule's ic_count /
     * ic_names side table (M4 follow-up — root chunk now carries IC sites
     * for top-level GETSLOT/SETSLOT).  proto = NULL for the root chunk. */
    init_ic_slice(&arr->entries[0], NULL,
                  m->ic_count, m->ic_names, &ic_cursor);

    /* entries[1..n-1]: parallel to module->nested[].  Each gets its own
     * slice of the trailing IC region; unfilled sites have topology_gen == 0
     * (the sentinel per pre-M4 topology-generation spec §3.1). */
    for (uint16_t i = 0U; i < m->nested_count; i++) {
        UProto *p = m->nested[i];
        uint16_t   pc = (p != NULL) ? p->ic_count : 0U;
        USymbol  **pn = (p != NULL) ? p->ic_names : NULL;
        init_ic_slice(&arr->entries[i + 1U], p, pc, pn, &ic_cursor);
    }

    /* Publish the bulk pointer last so a partial-init mi never hands a
     * half-formed UProtoInstanceArr to the walker. */
    mi->proto_instances = arr;

    /* T30: register on the per-VM linked list AFTER both cells are wired.
     * Insertion order at head — the determinism checksum walks every
     * instance and folds in IC state, so order is observable.  Caller's
     * create-order is itself deterministic in the test harness; therefore
     * the iteration order is stable across runs. */
    mi->next_in_vm = vm->module_instances_head;
    vm->module_instances_head = mi;

    return mi;
}

void
urbi_module_instance_destroy(struct UVM *vm, UModuleInstance *mi)
{
    /* GC-managed; sweep reaps both cells when no roots reach mi.  No
     * explicit teardown at T16 — IC entries hold no host-owned resources
     * yet (T22+ may revisit if IC fill installs anything that needs an
     * explicit finalizer). */
    (void)vm;
    (void)mi;
}

UModuleInstance *
urbi_get_or_create_module_instance(struct UVM *vm, UModule *m)
{
    if (vm == NULL || m == NULL) return NULL;
    UModuleInstance *mi;
    for (mi = vm->module_instances_head; mi != NULL; mi = mi->next_in_vm) {
        if (mi->module == m) return mi;
    }
    return urbi_module_instance_create(vm, m);
}
