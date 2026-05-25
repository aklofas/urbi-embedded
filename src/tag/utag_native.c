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
 *   Phase 7 (M6 stdlib) closed TAGCH-013 partially by removing the two
 *   getter stubs (`enter`/`leave`) that were unreachable from any caller
 *   — UVAL_TAG does not exist in UValKind, so OP_CALL could never dispatch
 *   them and C tests had no reason to invoke them indirectly.  When tag-
 *   property dispatch lands (post-M6 — M7 C-API or later) the proper
 *   UProps OGET/OSET path will reinstate scripted access via the tag
 *   getter helpers (tag_enter_getter / tag_leave_getter), still typed.
 *
 * tag_enter_leave_setter_protected:
 *   Any write to tag.enter or tag.leave raises URBI_ERR_PROTECTED_SLOT.
 *   A UVAL_HOST_FN setter stub is installed on the `_enter_set`/`_leave_set`
 *   slot names; reachable today from C-level tests via direct slot lookup.
 *   Full OP_SETSLOT getter/setter dispatch via UProps OGET/OSET lands when
 *   property dispatch lands. */

#include "tag/utag_native.h"

#include "vm/uvm.h"
#include "tag/utag.h"              /* UTag, tag->enter_event / leave_event */
#include "event/uevent.h"            /* UEvent, urbi_event_create */
#include "event/uevent_native.h"      /* uvalue_from_event */
#include "value/uintern.h"           /* ustr_intern */
#include "object/uobject.h"    /* urbi_object_alloc, urbi_object_install_property */
#include "runtime/umacros.h"   /* URBI_INTERNAL_ASSERT (TAGCH-002), urbi_strlen, urbi_zero */
#include "urbi/urbi.h"         /* URBI_ERR_PROTECTED_SLOT, URBI_ERR_OOM, UHostFn */
/* urbi_gc_slot_pre_store (Dijkstra forward barrier; barrier-only variant since
 * tag->enter_event / tag->leave_event are UEvent*, not UValue*, so the
 * combined urbi_gc_slot_store cannot be used here) is reached via urbi/gc.h
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
        urbi_gc_slot_pre_store(vm, (UCell *)tag, 0U, uvalue_from_event(e));
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
        urbi_gc_slot_pre_store(vm, (UCell *)tag, 0U, uvalue_from_event(e));
        tag->leave_event = e;
    }
    return uvalue_from_event(tag->leave_event);
}

/* === Native method stubs for proto slot installation === */

/* Protected setter stub for both enter and leave.
 *
 * TAGCH-013 (defer:M6, partial close at Phase 7): the two getter stubs
 * (tag_enter_getter_stub / tag_leave_getter_stub) were removed because they
 * were unreachable from any path — UVAL_TAG does not exist in UValKind so
 * neither OP_CALL nor any C-level test routed through them.  This setter
 * stub stays because tests/unit/test_tag_native.c still reaches it via a
 * direct UVAL_HOST_FN slot lookup to verify the URBI_ERR_PROTECTED_SLOT
 * raise behaviour.  When the proper UProps OGET/OSET dispatch path lands
 * (post-M6) this stub is replaced by typed getter/setter binding. */
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

/* === register_host_fn ===
 *
 * Install a UVAL_HOST_FN slot named `name` on `proto`.
 *
 * Phase 7 (M6 stdlib, EVENT-013 closure): pre-Phase-7 this lived as
 * `urbi_register_fn` in src/event/uevent_native.c, used by both event and
 * tag native registration.  Phase 7 migrated event registration to the
 * Phase-3 native-method ABI (UVAL_CLOSURE + native_fn) since OP_CALL only
 * dispatches UVAL_CLOSURE.  Tag still uses the host-fn path solely for the
 * `_enter_set`/`_leave_set` setter stubs which are unreachable from script
 * but tested directly from C — so the host-fn install helper survives here
 * as a private helper, not exported.  When tag-property dispatch lands
 * (post-M6) this whole code path is replaced by typed UProps installation. */
static int
register_host_fn(struct UVM *vm, struct UObject *proto,
                 const char *name, UHostFn fn)
{
    if (vm == NULL || proto == NULL || fn == NULL || name == NULL) {
        return -1;
    }
    USymbol *sym = (USymbol *)ustr_intern(vm, name, urbi_strlen(name));
    if (sym == NULL) {
        return -1;
    }
    UValue v;
    urbi_zero(&v, sizeof(v));
    v.kind = (uint8_t)UVAL_HOST_FN;
    /* Function-to-data pointer round-trip: defined for v1.0 targets (POSIX,
     * ARM Cortex-M7, RISC-V32, Xtensa LX7 — all Harvard-flat for code/data
     * within one address space). */
    v.v.p = (void *)(uintptr_t)fn;  /* NOLINT(performance-no-int-to-ptr) — UVAL_HOST_FN function-pointer encoding */
    return urbi_object_set_local_slot(vm, proto, sym, v);
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

    /* TAGCH-004: propagate slot-install failures.  The previous code
     * dropped the return values, so an OOM during slot intern/install
     * left a partially populated tag_proto on the VM.  Chain with || and
     * on any non-zero return clear vm->tag_proto and surface UVM_OOM.
     * The proto cell stays GC-managed and is collected at the next sweep.
     *
     * TAGCH-013 (Phase 7 partial close): the two getter stubs were
     * removed; only the protected setter stays installed (`_enter_set` /
     * `_leave_set`) until tag-property dispatch lands and replaces this
     * scaffold with typed UProps OGET/OSET binding. */
    if (register_host_fn(vm, proto, "_enter_set", tag_enter_leave_setter_protected) != 0
     || register_host_fn(vm, proto, "_leave_set", tag_enter_leave_setter_protected) != 0) {
        vm->tag_proto = NULL;
        return UVM_OOM;
    }
    return UVM_OK;
}
