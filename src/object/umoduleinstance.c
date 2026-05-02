/* SPDX-License-Identifier: BSD-3-Clause */
/* umoduleinstance.c — UModuleInstance + UProtoInstanceArr lifecycle.
 *
 * See umoduleinstance.h for the design contract and layout invariants. */

#include "object/umoduleinstance.h"

#include "urbi/gc.h"          /* urbi_gc_alloc, UTYPE_MODULE_INSTANCE, UTYPE_PROTO_INSTANCE */
#include "uvm.h"              /* UVM (for the typed pointer) */

#include <stddef.h>
#include <stdint.h>

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

    /* Cell 2: UProtoInstanceArr bulk.  Layout = [header pad] + entries[n] +
     * IC tables for every nested proto's ic_count.  entries[0] is the
     * root chunk (no ICs at v1.0); entries[1..n-1] mirror module->nested[].
     *
     * Bulk size = sizeof(UProtoInstanceArr)         (header + cell + pad)
     *           + n * sizeof(UProtoInstance)         (entries[] payload)
     *           + sum(nested[i]->ic_count) * sizeof(UIC)  (trailing IC region) */
    uint16_t n = (uint16_t)(1u + m->nested_count);

    size_t entries_bytes = (size_t)n * sizeof(UProtoInstance);

    size_t ic_bytes = 0u;
    for (size_t i = 0u; i < m->nested_count; i++) {
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
    arr->_pad[0]  = 0u;
    arr->_pad[1]  = 0u;
    arr->_pad[2]  = 0u;

    /* IC tables live immediately after entries[].  Compute base by
     * pointer-arithmetic past the FAM. */
    UIC *ic_cursor = (UIC *)(void *)((uint8_t *)arr->entries + entries_bytes);

    /* entries[0]: root chunk.  No IC sites at v1.0 (the root chunk doesn't
     * emit GETSLOT/SETSLOT today; module-level slot access is wrapped in a
     * function before reaching the IC path).  Leave proto NULL + ic_table
     * NULL — slot[0] is intentionally a sentinel. */
    arr->entries[0].proto    = NULL;
    arr->entries[0].ic_table = NULL;

    /* entries[1..n-1]: parallel to module->nested[].  Each gets its own
     * slice of the trailing IC region; zero-fill every entry so an unfilled
     * site has topology_gen == 0 (the "unfilled" sentinel per pre-M4
     * topology-generation spec §3.1; vm->topology_gen init=1 guarantees no
     * live shape ever has gen 0). */
    for (uint16_t i = 0u; i < m->nested_count; i++) {
        UProto *p = m->nested[i];
        UProtoInstance *pi = &arr->entries[i + 1u];
        pi->proto = p;

        if (p == NULL || p->ic_count == 0u) {
            pi->ic_table = NULL;
            continue;
        }

        pi->ic_table = ic_cursor;
        for (uint16_t k = 0u; k < p->ic_count; k++) {
            UIC *ic = &ic_cursor[k];
            ic->name           = (p->ic_names != NULL) ? p->ic_names[k] : NULL;
            ic->n              = 0u;
            ic->replace_cursor = 0u;
            for (int e = 0; e < URBI_IC_ENTRIES_PER_SITE; e++) {
                ic->recv_shapes[e]  = NULL;
                ic->topology_gen[e] = 0u;
                ic->slots[e]        = NULL;
                ic->uprops[e]       = NULL;
                ic->flags[e]        = 0u;
            }
        }
        ic_cursor += p->ic_count;
    }

    /* Publish the bulk pointer last so a partial-init mi never hands a
     * half-formed UProtoInstanceArr to the walker. */
    mi->proto_instances = arr;

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
