/* SPDX-License-Identifier: BSD-3-Clause */
/* utypes_init.c — UType descriptor registration for the M4 object-model
 * cell types.
 *
 * Built-in tags (1..UTYPE_HOST_BASE-1) cannot be registered through
 * urbi_register_type per src/utype.c §guard.  This file owns direct
 * vm->type_table[tag] = &descriptor writes for the M4 cell types and is
 * called from uvm_init after vm->type_table[] has been zeroed.
 *
 * Walker shape (per pre-M4 prototype-chain spec §6 + USlot/UProps spec §4):
 *   UObject  walks shape (direct UCell*) + each USlot UValue payload via cb +
 *            each prototype (direct UObject*) via UPROTOS_FOREACH (T9).
 *   UProtos  walks items[i] (direct UObject*; embeds UCell at offset 0).
 *   UShape   walks parent (UShape*) + transitions (UShapeMap*) +
 *            the UPropsTable wrapper cell (recovered via offsetof from
 *            props_table) + each props_table[i] (UProps*).  Skips name
 *            (USymbol — interned, never collected per intern-table contract).
 *   UShapeMap walks each entries[i].v (UShape*) when entry is occupied.
 *            Keys (USymbol*) are interned, never collected.
 *   UProps   walks oget + oset (UValue payloads) via cb.
 *   UPropsTable no-op: entries[] are reachable through the owning
 *            UShape walker (above).  The wrapper cell stays alive because
 *            UShape's walker shades it.
 *   USlotHandle / UModuleInstance / UProtoInstance — no-op walkers at this
 *            task.  Children traced once owning data lands (later M4 tasks).
 *
 * cb is the GC's own mark_root_callback (see src/gc/ugc_incremental.c) —
 * it knows how to shade only those UValKinds that carry a heap cell.  At
 * M3 baseline that's UVAL_CLOSURE only; UVAL_OBJECT/UVAL_STRING etc. are
 * extended in later M4/M5 tasks.  The walker calls are written today so
 * they automatically pick up the broader heap-bearing set when it lands. */

#include <stddef.h>           /* offsetof */
#include <stdint.h>

#include "object/uobject.h"
#include "object/ushape.h"
#include "object/umoduleinstance.h"
#include "object/utypes_init.h"
#include "gc/ugc.h"
#include "gc/ugc_incremental.h"   /* gc_shade_gray */
#include "uvm.h"

/* === walk_uobject ===
 *
 * Traces shape (direct UShape*), each USlot UValue payload via cb, and
 * each prototype (direct UObject*) via UPROTOS_FOREACH.  The macro
 * dispatches across the three storage forms of o->protos (empty/single/heap)
 * per pre-M4 prototype-chain spec §4.1. */
static void
walk_uobject(struct UVM *vm, void *payload,
             UGcRootCallback cb, void *ctx)
{
    UObject *o = (UObject *)((UCell *)payload - 1);

    /* shape is a direct UCell-headed object; shade via gc_shade_gray.
     * (The mark callback only handles UValue slots; for direct cell
     * pointers we go straight through gc_shade_gray, which is the same
     * shading routine the callback ultimately calls.) */
    if (o->shape != NULL) {
        gc_shade_gray(vm, (UCell *)o->shape);
    }

    /* Walk each USlot UValue payload via the mark callback.  The callback
     * checks uvalue_is_heap and shades the underlying cell if present.
     * USlot is a typedef for UValue (sizeof(USlot) == sizeof(UValue));
     * shape->count is the slot count when shape is non-NULL. */
    if (o->slots != NULL && o->shape != NULL) {
        uint32_t i;
        for (i = 0u; i < o->shape->count; i++) {
            cb(vm, &o->slots[i], ctx);
        }
    }

    /* Walk the prototype chain.  UPROTOS_FOREACH captures o->protos once
     * at iteration start and dispatches across empty/single/heap forms.
     * Entries are direct UObject* (embed UCell at offset 0). */
    {
        UObject *p;
        UPROTOS_FOREACH(o, p) {
            if (p != NULL) {
                gc_shade_gray(vm, (UCell *)p);
            }
        }
    }
}

/* === walk_uprotos ===
 *
 * Iterates the items[] flexible array.  n is the prototype count; entries
 * are direct UObject* (embed UCell at offset 0). */
static void
walk_uprotos(struct UVM *vm, void *payload,
             UGcRootCallback cb, void *ctx)
{
    (void)cb; (void)ctx;  /* direct-pointer walk doesn't go through cb */

    UProtos *up = (UProtos *)((UCell *)payload - 1);
    uint32_t i;
    for (i = 0u; i < up->n; i++) {
        if (up->items[i] != NULL) {
            gc_shade_gray(vm, (UCell *)up->items[i]);
        }
    }
}

/* === walk_ushape ===
 *
 * Traces parent (UShape*) + transitions (UShapeMap*) + props_table[i]
 * (UProps*).  Skips name (USymbol — interned, lives for the VM lifetime
 * per the intern-table contract; never collected). */
static void
walk_ushape(struct UVM *vm, void *payload,
            UGcRootCallback cb, void *ctx)
{
    (void)cb; (void)ctx;  /* direct-pointer walk doesn't go through cb */

    UShape *s = (UShape *)((UCell *)payload - 1);

    if (s->parent != NULL) {
        gc_shade_gray(vm, (UCell *)s->parent);
    }

    /* transitions is the per-shape UShapeMap (T13).  NULL until the first
     * slot is added out of this shape; non-NULL once any child shape has
     * been created.  walk_ushapemap (below) shades each cached child. */
    if (s->transitions != NULL) {
        gc_shade_gray(vm, (UCell *)s->transitions);
    }

    /* props_table is NULL until the first slot in this lineage installs
     * a property (per pre-M4 USlot/UProps spec §4.2).  When non-NULL it
     * points at the entries[] flexible array of a UPropsTable wrapper
     * cell allocated by urbi_shape_transition_property (T17).
     *
     * Reachability: shade both (a) the wrapper cell itself, recovered via
     * offsetof(UPropsTable, entries), and (b) each non-NULL UProps* entry.
     *
     * Test caveat: tests that synthesize props_table from a host-stack
     * array (test_ugc_object_cells.c Test 4) MUST allocate via the
     * UPropsTable wrapper to remain compatible with this walker. */
    if (s->props_table != NULL) {
        UPropsTable *pt = (UPropsTable *)(void *)
            ((uint8_t *)s->props_table - offsetof(UPropsTable, entries));
        gc_shade_gray(vm, (UCell *)pt);
        uint32_t i;
        for (i = 0u; i < s->count; i++) {
            if (s->props_table[i] != NULL) {
                gc_shade_gray(vm, (UCell *)s->props_table[i]);
            }
        }
    }
}

/* === walk_ushapemap ===
 *
 * Iterates the entries[] flexible array.  cap is the bucket count;
 * occupied entries (k != NULL) hold (USymbol*, UShape*).  Keys are
 * interned and not GC-managed — only values are shaded. */
static void
walk_ushapemap(struct UVM *vm, void *payload,
               UGcRootCallback cb, void *ctx)
{
    (void)cb; (void)ctx;  /* direct-pointer walk doesn't go through cb */

    UShapeMap *m = (UShapeMap *)((UCell *)payload - 1);
    uint32_t i;
    for (i = 0u; i < m->cap; i++) {
        if (m->entries[i].v != NULL) {
            gc_shade_gray(vm, (UCell *)m->entries[i].v);
        }
    }
}

/* === walk_uprops ===
 *
 * Traces oget + oset UValue payloads via the mark callback. */
static void
walk_uprops(struct UVM *vm, void *payload,
            UGcRootCallback cb, void *ctx)
{
    UProps *p = (UProps *)((UCell *)payload - 1);
    cb(vm, &p->oget, ctx);
    cb(vm, &p->oset, ctx);
}

/* === walk_noop ===
 *
 * No-op walker for cell types whose payload is fully described but whose
 * children-walk lands at a later M4 task (USlotHandle remains here at T16;
 * UModuleInstance + UProtoInstance get real walkers below). */
static void
walk_noop(struct UVM *vm, void *payload,
          UGcRootCallback cb, void *ctx)
{
    (void)vm; (void)payload; (void)cb; (void)ctx;
}

/* === walk_umoduleinstance (T16) ===
 *
 * Shades the UProtoInstanceArr bulk so it survives sweep as long as the
 * UModuleInstance is alive.  module is a non-owning pointer to a UModule
 * that lives outside the GC heap (flash-resident in freestanding builds;
 * caller-owned struct in hosted builds), so it's not shaded. */
static void
walk_umoduleinstance(struct UVM *vm, void *payload,
                     UGcRootCallback cb, void *ctx)
{
    (void)cb; (void)ctx;  /* direct-pointer walk doesn't go through cb */

    UModuleInstance *mi = (UModuleInstance *)((UCell *)payload - 1);
    if (mi->proto_instances != NULL) {
        gc_shade_gray(vm, (UCell *)mi->proto_instances);
    }
}

/* === walk_uprotoinstance (T16) ===
 *
 * No children to mark at T16: every UIC entry is zero-init
 * (recv_shapes / slots / uprops all NULL; topology_gen=0 = unfilled
 * sentinel).  TODO(T22+): once IC fill lands, walk each UIC.recv_shapes[e],
 * slots[e] (USlot UValue payload via cb), and uprops[e] for the live n
 * entries per site.  USymbol.name is interned and never collected. */
static void
walk_uprotoinstance(struct UVM *vm, void *payload,
                    UGcRootCallback cb, void *ctx)
{
    (void)vm; (void)payload; (void)cb; (void)ctx;
}

/* === Static UType descriptors ===
 *
 * payload_size is set to 0 (variable / not pinned at this task) for all
 * M4 types.  flags = 0 (no finalizer, not host-backed).  destroy = NULL
 * for every type at this task — finalizer integration lands when host
 * memory shows up in any of these payloads (none do today). */
static const UType type_uobject = {
    .type_tag      = UTYPE_OBJECT,
    .flags         = 0u,
    .payload_size  = 0u,
    .name          = "UObject",
    .walk_payload  = walk_uobject,
    .destroy       = NULL,
};

static const UType type_uprotos = {
    .type_tag      = UTYPE_PROTOS,
    .flags         = 0u,
    .payload_size  = 0u,
    .name          = "UProtos",
    .walk_payload  = walk_uprotos,
    .destroy       = NULL,
};

static const UType type_ushape = {
    .type_tag      = UTYPE_SHAPE,
    .flags         = 0u,
    .payload_size  = 0u,
    .name          = "UShape",
    .walk_payload  = walk_ushape,
    .destroy       = NULL,
};

static const UType type_ushapemap = {
    .type_tag      = UTYPE_SHAPE_MAP,
    .flags         = 0u,
    .payload_size  = 0u,
    .name          = "UShapeMap",
    .walk_payload  = walk_ushapemap,
    .destroy       = NULL,
};

static const UType type_uprops = {
    .type_tag      = UTYPE_PROPS,
    .flags         = 0u,
    .payload_size  = 0u,
    .name          = "UProps",
    .walk_payload  = walk_uprops,
    .destroy       = NULL,
};

/* UPropsTable walker is a no-op: the owning UShape's walker (above) iterates
 * each non-NULL entries[i] and shades the UProps cells.  The wrapper cell
 * itself stays alive because walk_ushape shades it via offsetof recovery. */
static const UType type_upropstable = {
    .type_tag      = UTYPE_PROPS_TABLE,
    .flags         = 0u,
    .payload_size  = 0u,
    .name          = "UPropsTable",
    .walk_payload  = walk_noop,
    .destroy       = NULL,
};

static const UType type_uslothandle = {
    .type_tag      = UTYPE_SLOTHANDLE,
    .flags         = 0u,
    .payload_size  = 0u,
    .name          = "USlotHandle",
    .walk_payload  = walk_noop,
    .destroy       = NULL,
};

static const UType type_umodule_instance = {
    .type_tag      = UTYPE_MODULE_INSTANCE,
    .flags         = 0u,
    .payload_size  = 0u,
    .name          = "UModuleInstance",
    .walk_payload  = walk_umoduleinstance,
    .destroy       = NULL,
};

static const UType type_uproto_instance = {
    .type_tag      = UTYPE_PROTO_INSTANCE,
    .flags         = 0u,
    .payload_size  = 0u,
    .name          = "UProtoInstance",
    .walk_payload  = walk_uprotoinstance,
    .destroy       = NULL,
};

/* === urbi_object_builtin_types_init ===
 *
 * Writes the M4 cell-type descriptors directly into vm->type_table[].
 * Built-in tags can't go through urbi_register_type (which guards against
 * tags < UTYPE_HOST_BASE per src/utype.c).  Called from uvm_init after
 * vm->type_table[] has been zeroed. */
void
urbi_object_builtin_types_init(struct UVM *vm)
{
    vm->type_table[UTYPE_OBJECT]          = (UType *)&type_uobject;
    vm->type_table[UTYPE_PROTOS]          = (UType *)&type_uprotos;
    vm->type_table[UTYPE_SHAPE]           = (UType *)&type_ushape;
    vm->type_table[UTYPE_SHAPE_MAP]       = (UType *)&type_ushapemap;
    vm->type_table[UTYPE_PROPS]           = (UType *)&type_uprops;
    vm->type_table[UTYPE_PROPS_TABLE]     = (UType *)&type_upropstable;
    vm->type_table[UTYPE_SLOTHANDLE]      = (UType *)&type_uslothandle;
    vm->type_table[UTYPE_MODULE_INSTANCE] = (UType *)&type_umodule_instance;
    vm->type_table[UTYPE_PROTO_INSTANCE]  = (UType *)&type_uproto_instance;
}
