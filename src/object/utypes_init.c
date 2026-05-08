/* SPDX-License-Identifier: BSD-3-Clause */
/* utypes_init.c — UType descriptor registration for the M4 object-model
 * cell types.
 *
 * Built-in tags (1..UTYPE_HOST_BASE-1) cannot be registered through
 * urbi_register_type per src/utype.c §guard.  This file owns direct
 * vm->type_table[tag] = &descriptor writes for the M4 cell types and is
 * called from urbi_vm_init after vm->type_table[] has been zeroed.
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
 *   USlotArray no-op: entries[] are reachable through the owning UObject's
 *            walker (which iterates o->slots[0..shape->count] via cb).
 *            The wrapper cell stays alive because walk_uobject shades it
 *            via offsetof(USlotArray, entries) recovery (T26).
 *   USlotHandle walks owner (UObject*); shape_at_create + name are
 *            reachable transitively through the owner.  T37.
 *   UModuleInstance / UProtoInstance — see walkers below.
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
#include "object/umodule_instance.h"
#include "object/uslothandle.h"   /* T37 — walk_uslothandle shades owner */
#include "object/utypes_init.h"
#include "event/uevent.h"               /* UEvent, UTYPE_EVENT (spec #3 §3.1) */
#include "changed/uchanged_node.h"        /* UChangedNode, UTYPE_CHANGED_NODE (spec #4 §3.1) */
#include "tag/utag.h"                 /* UTag, UTYPE_TAG (T18 GC promotion) */
#include "gc/ugc.h"
#include "gc/ugc_incremental.h"   /* gc_shade_gray */
#include "watcher/uwatcher.h"     /* UWatcher — for walk_uevent/utag chains */
#include "vm/uvm.h"

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
     * shape->count is the slot count when shape is non-NULL.  Also shade
     * the USlotArray wrapper cell itself (T26): o->slots points at the
     * entries[] FAM, so recover the wrapper base via offsetof.  Same trick
     * walk_ushape uses for props_table -> UPropsTable. */
    if (o->slots != NULL && o->shape != NULL) {
        UCell *wrapper = (UCell *)(void *)
            ((uint8_t *)o->slots - offsetof(USlotArray, entries));
        gc_shade_gray(vm, wrapper);
        uint32_t i;
        for (i = 0U; i < o->shape->count; i++) {
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

    /* Shade the slot-change subscriber chain (spec #4 §3.1).  NULL at alloc;
     * lazy-populated at first `obj.x.changed?` install (R6).  UChangedNode
     * embeds UCell as its first member, so the cast to UCell* is well-defined. */
    if (o->changed_events_head != NULL) {
        gc_shade_gray(vm, (UCell *)o->changed_events_head);
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
    for (i = 0U; i < up->n; i++) {
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
        for (i = 0U; i < s->count; i++) {
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
    for (i = 0U; i < m->cap; i++) {
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
 * children-walk lands at a later M4 task. */
static void
walk_noop(struct UVM *vm, void *payload,
          UGcRootCallback cb, void *ctx)
{
    (void)vm; (void)payload; (void)cb; (void)ctx;
}

/* === walk_uslothandle (T37) ===
 *
 * Strong reference to owner per pre-M4 USlot/UProps spec §7.6: shading
 * h->owner keeps the holding UObject alive as long as the handle is
 * reachable.  shape_at_create + name are reachable transitively through
 * h->owner (owner->shape's lineage carries USymbol* keys via UShape.name,
 * and USymbol is interned and never collected). */
static void
walk_uslothandle(struct UVM *vm, void *payload,
                 UGcRootCallback cb, void *ctx)
{
    (void)cb; (void)ctx;   /* direct-pointer walk doesn't go through cb */

    USlotHandle *h = (USlotHandle *)((UCell *)payload - 1);
    if (h->owner != NULL) {
        gc_shade_gray(vm, (UCell *)h->owner);
    }
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

/* === walk_uevent (spec #3 §3.1) ===
 *
 * Yields name (UValue payload via cb) and shades each UWatcher in the
 * at_watchers_head chain (direct UCell* walk — same pattern as walk_ushape
 * shading UProps* entries).
 *
 * waiters_head (UStrand chain) is intentionally NOT walked here: UStrands
 * are root-walked separately via the realm hierarchy (per M3 row 10 / GC
 * roots spec §5.3).  Walking them here would double-visit them and could
 * upset the write-barrier invariant during incremental mark. */
static void
walk_uevent(struct UVM *vm, void *payload,
            UGcRootCallback cb, void *ctx)
{
    UEvent *ev = (UEvent *)((UCell *)payload - 1);

    /* name is a UValue payload; route through cb so the mark callback
     * applies the heap-bearing check and shades the underlying cell if any. */
    cb(vm, &ev->name, ctx);

    /* Walk the at_watchers_head intrusive list.  UWatcher embeds UCell as
     * its first member (type_tag at offset 0), so the cast to UCell* is
     * well-defined (same as the UObject/UShape walkers above). */
    {
        UWatcher *w = ev->at_watchers_head;
        while (w != NULL) {
            gc_shade_gray(vm, (UCell *)w);
            w = w->next_in_event;
        }
    }
}

/* === walk_uchanged_node (spec #4 §3.1) ===
 *
 * Yields name (USymbol* — interned, never collected; NOT walked here) and
 * shades event (UEvent*) and next (UChangedNode*) as direct GC-managed cells.
 * The subscriber list is walked one node at a time: walk_uchanged_node is
 * called for each node, and each node shades only its own ->next link;
 * the GC traversal loop visits every grey cell in turn. */
static void
walk_uchanged_node(struct UVM *vm, void *payload,
                   UGcRootCallback cb, void *ctx)
{
    (void)cb; (void)ctx;  /* direct-pointer walk doesn't go through cb */

    UChangedNode *n = (UChangedNode *)((UCell *)payload - 1);

    /* name is interned (lives for VM lifetime); no shade needed. */

    if (n->event != NULL) {
        gc_shade_gray(vm, (UCell *)n->event);
    }
    if (n->next != NULL) {
        gc_shade_gray(vm, (UCell *)n->next);
    }
}

/* === walk_utag (T18 — spec #3 §3.4) ===
 *
 * Yields name (UValue payload via cb) and shades each UWatcher in the
 * member_watchers_head chain (direct UCell* walk — same pattern as
 * walk_uevent above).  Also shades enter_event and leave_event when
 * non-NULL (lazy-allocated by getter on first `at(tag.enter?)`).
 *
 * member_strands_head (UCleanupEntry chain) is intentionally NOT walked:
 * UStrands are root-walked separately via the realm hierarchy (M3 row 10 /
 * GC roots spec §5.3).  Walking them here would double-visit strands. */
static void
walk_utag(struct UVM *vm, void *payload,
          UGcRootCallback cb, void *ctx)
{
    UTag *t = (UTag *)((UCell *)payload - 1);

    /* name is a UValue payload; route through cb so the mark callback
     * applies the heap-bearing check and shades the underlying cell if any. */
    cb(vm, &t->name, ctx);

    /* enter_event and leave_event are UEvent* (direct UCell* walk). */
    if (t->enter_event != NULL) {
        gc_shade_gray(vm, (UCell *)t->enter_event);
    }
    if (t->leave_event != NULL) {
        gc_shade_gray(vm, (UCell *)t->leave_event);
    }

    /* Walk the member_watchers_head intrusive list.  UWatcher embeds UCell
     * as its first member (type_tag at offset 0), so the cast is well-defined
     * (same as the UObject/UShape walkers above). */
    {
        UWatcher *w = t->member_watchers_head;
        while (w != NULL) {
            gc_shade_gray(vm, (UCell *)w);
            w = w->next_in_tag;
        }
    }
}

/* === Static UType descriptors ===
 *
 * payload_size is set to 0 (variable / not pinned at this task) for all
 * M4 types.  flags = 0 (no finalizer, not host-backed).  destroy = NULL
 * for every type at this task — finalizer integration lands when host
 * memory shows up in any of these payloads (none do today). */
static const UType type_uobject = {
    .type_tag      = UTYPE_OBJECT,
    .flags         = 0U,
    .payload_size  = 0U,
    .name          = "UObject",
    .walk_payload  = walk_uobject,
    .destroy       = NULL,
};

static const UType type_uprotos = {
    .type_tag      = UTYPE_PROTOS,
    .flags         = 0U,
    .payload_size  = 0U,
    .name          = "UProtos",
    .walk_payload  = walk_uprotos,
    .destroy       = NULL,
};

static const UType type_ushape = {
    .type_tag      = UTYPE_SHAPE,
    .flags         = 0U,
    .payload_size  = 0U,
    .name          = "UShape",
    .walk_payload  = walk_ushape,
    .destroy       = NULL,
};

static const UType type_ushapemap = {
    .type_tag      = UTYPE_SHAPE_MAP,
    .flags         = 0U,
    .payload_size  = 0U,
    .name          = "UShapeMap",
    .walk_payload  = walk_ushapemap,
    .destroy       = NULL,
};

static const UType type_uprops = {
    .type_tag      = UTYPE_PROPS,
    .flags         = 0U,
    .payload_size  = 0U,
    .name          = "UProps",
    .walk_payload  = walk_uprops,
    .destroy       = NULL,
};

/* UPropsTable walker is a no-op: the owning UShape's walker (above) iterates
 * each non-NULL entries[i] and shades the UProps cells.  The wrapper cell
 * itself stays alive because walk_ushape shades it via offsetof recovery. */
static const UType type_upropstable = {
    .type_tag      = UTYPE_PROPS_TABLE,
    .flags         = 0U,
    .payload_size  = 0U,
    .name          = "UPropsTable",
    .walk_payload  = walk_noop,
    .destroy       = NULL,
};

/* USlotArray walker is a no-op: entries[] are reachable through the owning
 * UObject's walker (which iterates o->slots[0..shape->count] via cb).  The
 * wrapper cell itself stays alive because walk_uobject shades it via
 * offsetof recovery (T26). */
static const UType type_uslot_array = {
    .type_tag      = UTYPE_SLOT_ARRAY,
    .flags         = 0U,
    .payload_size  = 0U,
    .name          = "USlotArray",
    .walk_payload  = walk_noop,
    .destroy       = NULL,
};

static const UType type_uslothandle = {
    .type_tag      = UTYPE_SLOTHANDLE,
    .flags         = 0U,
    .payload_size  = 0U,
    .name          = "USlotHandle",
    .walk_payload  = walk_uslothandle,
    .destroy       = NULL,
};

static const UType type_umodule_instance = {
    .type_tag      = UTYPE_MODULE_INSTANCE,
    .flags         = 0U,
    .payload_size  = 0U,
    .name          = "UModuleInstance",
    .walk_payload  = walk_umoduleinstance,
    .destroy       = NULL,
};

/* UProtoInstance walker is a no-op: every UIC entry's children
 * (recv_shapes[e], slots[e], uprops[e]) are reachable through stronger
 * paths (UModuleInstance owns the UProtoInstanceArr; UShapes used by
 * IC entries are kept alive via walk_ushape from the receiver-side
 * UObject; UProps cells are kept alive via walk_ushape's props_table
 * walk).  The previous walk_uprotoinstance function was an explicit
 * no-op stub with a stale TODO; retired in v0.5.7-fixes Phase 13
 * (OBJ-028) — substituted by walk_noop. */
static const UType type_uproto_instance = {
    .type_tag      = UTYPE_PROTO_INSTANCE,
    .flags         = 0U,
    .payload_size  = 0U,
    .name          = "UProtoInstance",
    .walk_payload  = walk_noop,
    .destroy       = NULL,
};

static const UType type_uevent = {
    .type_tag      = UTYPE_EVENT,
    .flags         = 0U,
    .payload_size  = 0U,
    .name          = "UEvent",
    .walk_payload  = walk_uevent,
    .destroy       = NULL,
};

static const UType type_uchanged_node = {
    .type_tag      = UTYPE_CHANGED_NODE,
    .flags         = 0U,
    .payload_size  = 0U,
    .name          = "UChangedNode",
    .walk_payload  = walk_uchanged_node,
    .destroy       = NULL,
};

static const UType type_utag = {
    .type_tag      = UTYPE_TAG,
    .flags         = 0U,
    .payload_size  = 0U,
    .name          = "UTag",
    .walk_payload  = walk_utag,
    .destroy       = NULL,
};

/* === urbi_object_builtin_types_init ===
 *
 * Writes the M4 cell-type descriptors directly into vm->type_table[].
 * Built-in tags can't go through urbi_register_type (which guards against
 * tags < UTYPE_HOST_BASE per src/utype.c).  Called from urbi_vm_init after
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
    vm->type_table[UTYPE_SLOT_ARRAY]      = (UType *)&type_uslot_array;
    vm->type_table[UTYPE_SLOTHANDLE]      = (UType *)&type_uslothandle;
    vm->type_table[UTYPE_MODULE_INSTANCE] = (UType *)&type_umodule_instance;
    vm->type_table[UTYPE_PROTO_INSTANCE]  = (UType *)&type_uproto_instance;
    vm->type_table[UTYPE_EVENT]           = (UType *)&type_uevent;
    vm->type_table[UTYPE_CHANGED_NODE]    = (UType *)&type_uchanged_node;
    vm->type_table[UTYPE_TAG]             = (UType *)&type_utag;
}
