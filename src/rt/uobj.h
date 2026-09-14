/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/uobj.h — objects, slots, and prototype lookup for the refound/core
 * runtime. No shapes, transition trees, or inline caches: slots are a
 * linear-scanned parallel-array vector, and lookup through the proto chain
 * is a depth-first walk guarded by a per-VM visit stamp for diamonds. */

#ifndef URT_OBJ_H
#define URT_OBJ_H
#include "rt/ustr.h"

#define USLOT_CONSTANT      0x01
#define USLOT_GETTER        0x02
#define USLOT_SETTER        0x04
#define USLOT_CHANGED_EVENT 0x08
#define UOBJ_F_READONLY     0x0001   /* in cell.flags: slot writes throw */
#define UOBJ_F_FROZEN       0x0002   /* proto list immutable */
#define UOBJ_F_IS_PROTO     0x0004
/* Sticky: some watcher condition has read a slot on this object, so every
 * later write to it has to mark the dirty set.  Set once by the reactive
 * task and never cleared -- an object a condition looked at once is
 * assumed interesting for the life of the VM, which is what "no
 * per-watcher read sets" costs and buys. */
#define UOBJ_F_WATCHED      0x0008
/* Somebody has taken `x.changed?` on at least one of this object's slots,
 * so a write that INSTALLS a slot has to check whether that slot is the
 * subscribed one.  Writes to objects without the bit cost nothing. */
#define UOBJ_F_CHANGE_EVENTS 0x0010

/* Value-array entry for a slot with GETTER and/or SETTER set: values[i]
 * holds a UValue of kind UV_CELL pointing at one of these instead of the
 * plain value. `value` is the slot's own payload (what uobj_set_local's
 * caller passed as v); getter/setter are populated by whatever later
 * layer wires up accessor closures -- uobj.c only allocates the cell and
 * stores `value`, it never calls a getter or setter. */
typedef struct UProps { UCell cell; UValue getter, setter, value; } UProps;

typedef struct UObject {
    UCell     cell;
    USym    **names;
    UValue   *values;
    uint8_t  *attrs;
    uint16_t  count, cap;
    uint16_t  nprotos;
    struct UObject  *proto0;
    struct UObject **protos;     /* used when nprotos > 1; NULL otherwise */
    uint32_t  id;
    uint32_t  visit;             /* per-VM stamp for diamond-safe walks */
} UObject;

typedef struct UObjStats {
    uint32_t next_id, visit_stamp;
    /* Set when the last uobj_resolve / uobj_is_a ran out of walk stack
     * (URESOLVE_STACK_CAP, 64 frontier entries) and gave up.  Both return
     * "not found" in that case, which for a legal-but-huge proto graph is
     * a wrong answer rather than an answer -- so the message a caller
     * raises has to say which of the two it got.  Cleared at the top of
     * every walk; read through uobj_resolve_overflowed. */
    uint8_t  resolve_overflow;
} UObjStats;
UObjStats *uvm_objstats(struct UVM *vm);      /* defined in uexec.c; see tests/rt/fakevm.c */

static inline UValue uv_obj(UObject *o) { return uv_ptr(UV_OBJ, o); }

/* proto may be NULL. May collect (via ugc_alloc); root `proto` first if
 * it isn't otherwise reachable. */
UObject *uobj_new(struct UVM *vm, UObject *proto);
int      uobj_add_proto(struct UVM *vm, UObject *o, UObject *p);   /* PREPEND; 0 ok, -1 OOM */
int      uobj_remove_proto(struct UVM *vm, UObject *o, const UObject *p); /* 0 ok, -1 not found */
int      uobj_set_protos(struct UVM *vm, UObject *o, UObject **ps, uint16_t n); /* copies ps[]; 0 ok, -1 OOM */
/* Local slots */
int      uobj_find_local(const UObject *o, const USym *name);            /* index or -1 */
/* Add or overwrite; returns index or -1 OOM. When attrs has GETTER or
 * SETTER set, allocates a UProps cell (may collect -- `o` must already be
 * rooted by the caller) and stores v as the cell's `value` field. */
int      uobj_set_local(struct UVM *vm, UObject *o, USym *name, UValue v, uint8_t attrs);
bool     uobj_remove_local(struct UVM *vm, UObject *o, const USym *name);
/* Resolution through protos (depth-first, diamond-safe). */
typedef struct UObjSlotRef { UObject *owner; int index; } UObjSlotRef;
bool     uobj_resolve(struct UVM *vm, UObject *o, const USym *name, UObjSlotRef *out);
static inline UValue  uobj_slot_value(const UObjSlotRef *r) { return r->owner->values[r->index]; }
static inline uint8_t uobj_slot_attrs(const UObjSlotRef *r) { return r->owner->attrs[r->index]; }
bool     uobj_is_a(struct UVM *vm, UObject *o, UObject *proto);    /* o == proto or proto in o's ancestry */
/* Whether the walk that just returned false ran out of stack rather than
 * searching the whole graph.  Valid until the next walk. */
bool     uobj_resolve_overflowed(struct UVM *vm);
void     uobj_trace(struct UVM *vm, UObject *o);                   /* GC: mark names? no (immortal), values, protos */
void     uobj_finalize(struct UVM *vm, UObject *o);                /* free the three arrays and protos[] */
#endif
