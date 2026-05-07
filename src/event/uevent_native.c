/* SPDX-License-Identifier: BSD-3-Clause */
/* Event prototype native methods (spec #3 §7.3).
 *
 * Installs four native slots on vm->event_proto at uvm_init time:
 *   new        — allocate a fresh UEvent via urbi_event_create
 *   emit       — async fan-out via c_event_emit_async
 *   syncEmit   — sync fan-out via c_event_emit_sync
 *   waituntil  — block caller strand via c_event_waituntil
 *
 * Native slots are stored as UVAL_HOST_FN values inside urbi_register_fn.
 * OP_CALL dispatch wires into these once the proto lookup resolves a
 * UVAL_HOST_FN (T56 onwards); at T53 the proto slots exist and the native
 * functions are callable directly from C-level tests.
 *
 * urbi_register_fn — minimal helper: interns the slot name, builds a
 * UVAL_HOST_FN UValue, and installs it as a local slot on the proto.
 * Lives here rather than a shared header because only T53/T54 use it
 * and moving it to a shared header would pull uevent_emit.h + uintern.h
 * into every consumer of urbi.h.  Factor out at M6 if a third caller appears. */

#include "event/uevent_native.h"

#include "vm/uvm.h"
#include "sched/ustrand.h"           /* UStrand (was transitively pulled via urbi.h pre-v0.5.5) */
#include "value/uintern.h"           /* ustr_intern */
#include "event/uevent.h"            /* UEvent, urbi_event_create */
#include "event/uevent_emit.h"       /* c_event_emit_async, c_event_emit_sync, c_event_waituntil */
#include "object/uobject.h"    /* urbi_object_alloc, urbi_object_set_local_slot */
#include "urbi/urbi.h"         /* UErrCode, URBI_ERR_* */
#include "urbi/gc.h"           /* gc_shade_gray (write barrier in set_local_slot) */
#include "runtime/umacros.h"   /* urbi_zero */

#include <stddef.h>  /* size_t */

/* === uvalue_from_event / uvalue_as_event ===
 *
 * UEvent is a GC-managed cell.  At M5 we add UVAL_EVENT (kind=9) to the
 * UValKind enum (umodule.h) so the GC barrier in uvalue_is_heap() shades it.
 * Both helpers are defined here (internal to src/) rather than in a public
 * header — embedders access events through the C API, not raw UValue tags. */

UValue
uvalue_from_event(UEvent *e)
{
    UValue v;
    urbi_zero(&v, sizeof(v));
    v.kind  = (uint8_t)UVAL_EVENT;
    v.v.p   = (void *)e;
    return v;
}

UEvent *
uvalue_as_event(UValue v)
{
    return (UEvent *)v.v.p;
}

int
uvalue_is_event(UValue v)
{
    return v.kind == (uint8_t)UVAL_EVENT;
}

/* === native_event_optional_payload ===
 *
 * Extract the optional payload from a native method argument list.
 * Used by urbi_native_event_emit and urbi_native_event_sync_emit.
 * Returns argv[1] when present, or a NIL UValue when argc < 2. */
static UValue
native_event_optional_payload(int argc, UValue *argv)
{
    if (argc > 1) {
        return argv[1];
    }
    UValue nil;
    urbi_zero(&nil, sizeof(nil));
    nil.kind = (uint8_t)UVAL_NIL;
    return nil;
}

/* === urbi_register_fn ===
 *
 * Install a UVAL_HOST_FN slot named `name` on `proto`.
 * Interns `name` → USymbol; builds a UVAL_HOST_FN UValue storing fn in v.p;
 * calls urbi_object_set_local_slot to add the slot.
 *
 * Returns 0 on success, -1 on OOM (either intern or slot-alloc failed).
 * Silent no-op if vm or proto or fn is NULL. */
int
urbi_register_fn(struct UVM *vm, struct UObject *proto,
                 const char *name, UHostFn fn)
{
    size_t n;
    if (vm == NULL || proto == NULL || fn == NULL || name == NULL) {
        return -1;
    }
    /* Freestanding-safe strlen: avoid <string.h> dependency. */
    n = 0;
    while (name[n] != '\0') n++;
    USymbol *sym = (USymbol *)ustr_intern(vm, name, n);
    if (sym == NULL) {
        return -1;
    }
    UValue v;
    urbi_zero(&v, sizeof(v));
    v.kind  = (uint8_t)UVAL_HOST_FN;
    v.v.p   = (void *)(uintptr_t)fn;  /* store function pointer as void* */
    return urbi_object_set_local_slot(vm, proto, sym, v);
}

/* === Native method implementations === */

/* urbi_native_event_new: constructor — allocates a fresh UEvent.
 *
 * Argv[0] is the receiver (the Event proto); ignored.
 * Returns a UVAL_EVENT wrapping the new UEvent, or NIL on OOM. */
static UValue
urbi_native_event_new(struct UStrand *s, int argc, UValue *argv)
{
    (void)argc; (void)argv;
    struct UVM *vm = s->vm;
    UEvent *e = urbi_event_create(vm);
    if (e == NULL) {
        UValue nil = {0};
        nil.kind = (uint8_t)UVAL_NIL;
        return nil;
    }
    return uvalue_from_event(e);
}

/* urbi_native_event_emit: async emit.
 *
 * Argv[0] = receiver (UEvent); argv[1] = optional payload.
 * Returns NIL. */
static UValue
urbi_native_event_emit(struct UStrand *s, int argc, UValue *argv)
{
    struct UVM *vm = s->vm;
    UEvent *e = uvalue_as_event(argv[0]);
    c_event_emit_async(vm, e, native_event_optional_payload(argc, argv));
    UValue nil = {0};
    nil.kind = (uint8_t)UVAL_NIL;
    return nil;
}

/* urbi_native_event_sync_emit: synchronous emit.
 *
 * Same shape as urbi_native_event_emit but calls c_event_emit_sync. */
static UValue
urbi_native_event_sync_emit(struct UStrand *s, int argc, UValue *argv)
{
    struct UVM *vm = s->vm;
    UEvent *e = uvalue_as_event(argv[0]);
    c_event_emit_sync(vm, e, native_event_optional_payload(argc, argv));
    UValue nil = {0};
    nil.kind = (uint8_t)UVAL_NIL;
    return nil;
}

/* urbi_native_event_waituntil: block caller strand until event fires.
 *
 * Argv[0] = receiver (UEvent).
 * Returns the payload deposited by the emitting strand on wake. */
static UValue
urbi_native_event_waituntil(struct UStrand *s, int argc, UValue *argv)
{
    (void)argc;
    struct UVM *vm = s->vm;
    UEvent *e = uvalue_as_event(argv[0]);
    return c_event_waituntil(vm, e);
}

/* === event_native_register ===
 *
 * Allocate vm->event_proto as a UObject in the URBI_ATOM_EVENT family,
 * then install the four native slots.  Called from uvm_init after the
 * type-table setup and atom-proto walk are in place.
 * Returns UVM_OK on success, UVM_OOM if the proto object allocation fails. */
UVMError
event_native_register(struct UVM *vm)
{
    /* Allocate the event proto object.  Use URBI_ATOM_EVENT as the family
     * so IC entries and atom-family checks behave correctly.
     * urbi_object_alloc sets the root shape; proto chain is empty at birth
     * (atom_event set_protos_single happens separately at M6 stdlib). */
    UObject *proto = urbi_object_alloc(vm, URBI_ATOM_EVENT);
    if (proto == NULL) {
        return UVM_OOM;
    }
    vm->event_proto = proto;

    urbi_register_fn(vm, proto, "new",       urbi_native_event_new);
    urbi_register_fn(vm, proto, "emit",      urbi_native_event_emit);
    urbi_register_fn(vm, proto, "syncEmit",  urbi_native_event_sync_emit);
    urbi_register_fn(vm, proto, "waituntil", urbi_native_event_waituntil);
    return UVM_OK;
}
