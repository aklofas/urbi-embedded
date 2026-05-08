/* SPDX-License-Identifier: BSD-3-Clause */
/* Tag.enter / Tag.leave native getters with lazy alloc (spec #3 §8.2).
 *
 * tag_enter_getter / tag_leave_getter:
 *   First read allocates a UEvent and stores it in tag->enter_event /
 *   tag->leave_event.  Subsequent reads return the cached pointer.
 *   Lazy allocation keeps tags free of the extra UEvent until something
 *   subscribes — matters for the typical hot-path tag (used for cleanup,
 *   never observed).
 *
 * Native getter binding on vm->tag_proto:
 *   urbi_register_native_getter does not yet exist as a general infrastructure
 *   piece.  At T54 we use urbi_register_fn (from event_native.h) to install
 *   thin UVAL_HOST_FN wrapper stubs that unpack the tag from argv[0] and
 *   delegate to the typed getters.  The getter-specific wrapper approach is
 *   the minimum-viable binding mechanism; the full native-getter IC path
 *   (OGET flag + UProps integration for OP_GETSLOT dispatch) is deferred to
 *   the M5 slot-change / M6 stdlib tasks when IC-aware property dispatch
 *   lands.
 *
 *   For now: OP_GETSLOT will find a UVAL_HOST_FN in the slot and dispatch
 *   via OP_CALL (once OP_CALL handles UVAL_HOST_FN, T56).  C-level tests
 *   call tag_enter_getter / tag_leave_getter directly.
 *
 * tag_enter_leave_setter_protected:
 *   Any write to tag.enter or tag.leave raises URBI_ERR_PROTECTED_SLOT.
 *   A UVAL_HOST_FN setter stub is installed on the same slot name via
 *   urbi_object_install_property (OSET flag).  At T54 this stub is
 *   reachable from C tests; full OP_SETSLOT getter/setter dispatch via
 *   UProps OGET/OSET lands at M6. */

#include "tag/utag_native.h"

#include "vm/uvm.h"
#include "tag/utag.h"              /* UTag, tag->enter_event / leave_event */
#include "event/uevent.h"            /* UEvent, urbi_event_create */
#include "event/uevent_native.h"      /* uvalue_from_event, urbi_register_fn */
#include "value/uintern.h"           /* ustr_intern */
#include "object/uobject.h"    /* urbi_object_alloc, urbi_object_install_property */
#include "runtime/umacros.h"   /* URBI_INTERNAL_ASSERT (TAGCH-002) */
#include "urbi/urbi.h"         /* URBI_ERR_PROTECTED_SLOT, URBI_ERR_OOM */
/* urbi_gc_slot_write (Dijkstra forward barrier) is reached via urbi/gc.h
 * pulled in by vm/uvm.h above. */
#include "sched/ustrand.h"           /* UStrand (for URBI_ERR_* throw helpers) */
#include <stddef.h>
#include <stdint.h>

/* === throw_oom_for_tag_event ===
 *
 * Shared OOM-throw path for tag_enter_getter / tag_leave_getter.
 * If vm->cur_strand is non-NULL (normal dispatch), throws URBI_ERR_OOM.
 * Returns a NIL UValue for use as the getter's return value on failure. */
static UValue
throw_oom_for_tag_event(struct UVM *vm)
{
    if (vm->cur_strand != NULL) {
        UValue err;
        err.kind = (uint8_t)UVAL_INT;
        err.v.i  = (int64_t)URBI_ERR_OOM;
        urbi_throw(vm->cur_strand, err);
    }
    UValue nil = {0};
    nil.kind = (uint8_t)UVAL_NIL;
    return nil;
}

/* === Lazy-alloc getter helpers === */

UValue
tag_enter_getter(struct UVM *vm, struct UTag *tag)
{
    /* TAGCH-016: lazy-alloc path drives urbi_gc_alloc via urbi_event_create —
     * not ISR-safe.  Mirror src/changed/uchanged.c:32. */
    URBI_ASSERT_NOT_ISR(vm);
    if (tag->enter_event == NULL) {
        UEvent *e = urbi_event_create(vm);
        if (e == NULL) return throw_oom_for_tag_event(vm);
        /* TAGCH-001: Dijkstra forward barrier on the enter_event field
         * write.  If the UTag is BLACK and the freshly-allocated UEvent
         * is white (it always is at birth — current_white via gc_alloc),
         * shade the event gray so the mark phase reaches it.  UTag has
         * a UCell header at offset 0 (UTYPE_TAG); the cast is sound.
         * key=0 (no slot index — UTag isn't a UObject; UGC_HAS_WATCHER_
         * OBSERVER is never set on UTag, so observer_dirty isn't fired). */
        urbi_gc_slot_write(vm, (UCell *)tag, 0U, uvalue_from_event(e));
        tag->enter_event = e;
    }
    return uvalue_from_event(tag->enter_event);
}

UValue
tag_leave_getter(struct UVM *vm, struct UTag *tag)
{
    /* TAGCH-016: lazy-alloc path — see tag_enter_getter rationale above. */
    URBI_ASSERT_NOT_ISR(vm);
    if (tag->leave_event == NULL) {
        UEvent *e = urbi_event_create(vm);
        if (e == NULL) return throw_oom_for_tag_event(vm);
        /* TAGCH-001: Dijkstra forward barrier on the leave_event field
         * write — same rationale as tag_enter_getter above. */
        urbi_gc_slot_write(vm, (UCell *)tag, 0U, uvalue_from_event(e));
        tag->leave_event = e;
    }
    return uvalue_from_event(tag->leave_event);
}

/* === Native method stubs for proto slot installation === */

/* Enter getter stub: receiver-cast site.
 *
 * TAGCH-002: at the M5 baseline this stub is unreachable through OP_CALL
 * dispatch — UVAL_TAG does not exist in UValKind (see include/urbi/types.h)
 * and tag-typed receivers can only land here via test code that hand-builds
 * argv[0].v.p.  The earlier shape `(UTag *)argv[0].v.p` was a type-unsafe
 * receiver cast that would silently mis-dispatch under any future caller
 * routed through OP_CALL with a wrong argv[0] kind.  Replace with an
 * URBI_INTERNAL_ASSERT(0) that aborts in URBI_DEBUG and falls through to
 * NIL in release.  When M6 lands first-class tag-typed receivers, this
 * stub is replaced by a properly-typed dispatch path; until then the
 * unreachability is part of the contract. */
static UValue
tag_enter_getter_stub(struct UStrand *s, int argc, UValue *argv)
{
    (void)s; (void)argc; (void)argv;
    URBI_INTERNAL_ASSERT(0 && "unreachable: tag_enter_getter_stub");
    UValue nil = {0};
    nil.kind = (uint8_t)UVAL_NIL;
    return nil;
}

/* Leave getter stub: same TAGCH-002 contract as the enter stub. */
static UValue
tag_leave_getter_stub(struct UStrand *s, int argc, UValue *argv)
{
    (void)s; (void)argc; (void)argv;
    URBI_INTERNAL_ASSERT(0 && "unreachable: tag_leave_getter_stub");
    UValue nil = {0};
    nil.kind = (uint8_t)UVAL_NIL;
    return nil;
}

/* Protected setter stub for both enter and leave. */
static UValue
tag_enter_leave_setter_protected(struct UStrand *s, int argc, UValue *argv)
{
    (void)argc; (void)argv;
    UValue err;
    err.kind = (uint8_t)UVAL_INT;
    err.v.i  = (int64_t)URBI_ERR_PROTECTED_SLOT;
    urbi_throw(s, err);
    UValue nil = {0};
    nil.kind = (uint8_t)UVAL_NIL;
    return nil;
}

/* === tag_native_register === */

UVMError
tag_native_register(struct UVM *vm)
{
    /* TAGCH-016: drives urbi_object_alloc + urbi_register_fn slot installs,
     * neither of which is ISR-safe.  Mirror src/changed/uchanged.c:32. */
    URBI_ASSERT_NOT_ISR(vm);
    UObject *proto = urbi_object_alloc(vm, URBI_ATOM_TAG);
    if (proto == NULL) {
        return UVM_OOM;   /* OOM: leave tag_proto NULL */
    }
    vm->tag_proto = proto;

    /* TAGCH-004: propagate urbi_register_fn failures.  Mirrors the
     * Phase-11 EVENT-005 pattern in event_native_register: the previous
     * code dropped the four return values, so an OOM during slot
     * intern/install left a partially populated tag_proto on the VM —
     * lookups for the missing slot names would return UVAL_NIL and
     * silently mis-dispatch.  Chain with || short-circuit; on any
     * non-zero return clear vm->tag_proto and surface UVM_OOM.  The
     * proto cell itself stays GC-managed and is collected at the next
     * sweep.
     *
     * Full OGET/OSET property dispatch via urbi_object_install_property
     * (which sets URBI_SLOT_FLAG_OGET and wires UProps) requires the
     * IC-aware property infrastructure to be plumbed through OP_GETSLOT's
     * uprops[] fast path.  For now, plain UVAL_HOST_FN slots work for
     * C-level tests and basic slot reads. */
    if (urbi_register_fn(vm, proto, "enter",      tag_enter_getter_stub)            != 0
     || urbi_register_fn(vm, proto, "leave",      tag_leave_getter_stub)            != 0
     || urbi_register_fn(vm, proto, "_enter_set", tag_enter_leave_setter_protected) != 0
     || urbi_register_fn(vm, proto, "_leave_set", tag_enter_leave_setter_protected) != 0) {
        vm->tag_proto = NULL;
        return UVM_OOM;
    }
    return UVM_OK;
}
