/* SPDX-License-Identifier: BSD-3-Clause */
/* Event prototype native methods (spec #3 §7.3).
 *
 * Installs four native slots on vm->event_proto at urbi_vm_init time:
 *   new        — allocate a fresh UEvent via urbi_event_create
 *   emit       — async fan-out via c_event_emit_async
 *   syncEmit   — sync fan-out via c_event_emit_sync
 *   waituntil  — block caller strand via c_event_waituntil
 *
 * Phase 7 (M6 stdlib): native slots are now installed as UVAL_CLOSURE values
 * carrying a `native_fn` pointer (the M6 Phase-3 native-method ABI), so
 * scripted `Event.new()`, `e.emit(p)`, `e.syncEmit(p)`, and `e.waituntil()`
 * all dispatch through OP_CALL's native-method branch.
 *
 * Pre-Phase-7 baseline used UVAL_HOST_FN slots installed via urbi_register_fn,
 * which OP_CALL never dispatched (it required UVAL_CLOSURE).  C-level tests
 * called the host-fn pointers directly — useful for unit testing the emit
 * primitives but not reachable from script.
 *
 * EVENT-013 (defer:M6): urbi_register_fn was hoisted out of event-specific
 * code at Phase 7 by retiring it altogether.  The Phase-3 native-method
 * registration helper (urbi_native_closure_create) is the sole installer
 * for native methods on atom protos now.  tag_native_register still uses
 * the legacy host-fn path for its (still-stub) enter/leave slots; that is
 * tracked by TAGCH-013 and lands when tag-property dispatch lands.
 *
 * EVENT-026 — exemption from v0.5.2 AST_AT_EVENT register-allocation fix:
 *   The native emit/syncEmit/waituntil paths bypass AST_AT_EVENT codegen
 *   entirely.  They consume already-resolved UValues from argv and call
 *   straight into c_event_emit_async / c_event_emit_sync / c_event_waituntil
 *   from C — no emitter freereg / next_reg state to keep in sync.  The
 *   register-allocation desync that affected scripted emit (v0.5.2 T6;
 *   fixed at TWO sibling sites in src/emit/uemit_react.c, AST_AT_EVENT
 *   sync+async + AT_SLOT_CHANGE) does not apply here. */

#include "event/uevent_native.h"

#include "vm/uvm.h"
#include "sched/ustrand.h"           /* UStrand (was transitively pulled via urbi.h pre-v0.5.5) */
#include "value/uintern.h"           /* ustr_intern */
#include "event/uevent.h"            /* UEvent, urbi_event_create */
#include "event/uevent_emit.h"       /* c_event_emit_async, c_event_emit_sync, c_event_waituntil */
#include "object/uobject.h"    /* urbi_object_alloc, urbi_object_set_local_slot */
#include "stdlib/object_root.h" /* urbi_native_closure_create, urbi_raise_* */
#include "runtime/uclosure.h"  /* struct UClosure for UVAL_CLOSURE registration */
#include "urbi/urbi.h"         /* UErrCode, URBI_ERR_* */
#include "urbi/types.h"        /* urbi_value_nil */
#include "urbi/gc.h"           /* gc_shade_gray (write barrier in set_local_slot) */
#include "runtime/umacros.h"   /* urbi_zero, urbi_strlen */

#include <stddef.h>  /* size_t */
#include "module/umodule.h"
#include <stdint.h>

/* === uvalue_from_event / uvalue_as_event / uvalue_is_event ===
 *
 * UEvent is a GC-managed cell.  At M5 we added UVAL_EVENT (kind=9) to
 * the UValKind enum (umodule.h) so the GC barrier in uvalue_is_heap()
 * shades it.  Phase-18 (Wave 6, 2026-05-09) made the three helpers
 * `static inline` in uevent_native.h to elide call overhead at the 5
 * in-tree call sites; no out-of-line definitions are needed. */

/* === event_optional_payload ===
 *
 * Extract the optional payload from a native method argument list.
 * Used by event_emit_method and event_sync_emit_method.
 * Returns args[0] when present, or a NIL UValue when nargs < 1. */
static UValue
event_optional_payload(uint8_t nargs, UValue *args)
{
    if (nargs >= 1) {
        return args[0];
    }
    return urbi_value_nil();
}

/* === register_native_method ===
 *
 * Install a UVAL_CLOSURE slot named `name` on `proto`, where the closure's
 * `native_fn` field holds `fn`.  OP_CALL dispatches to fn directly when this
 * slot is the callee.
 *
 * Returns URBI_OK on success, URBI_ERR_OOM on alloc/intern failure, and
 * URBI_ERR_INVALID_ARG on NULL inputs.
 *
 * Phase 7 (M6 stdlib): replaces the legacy urbi_register_fn UVAL_HOST_FN
 * installer.  This preserves the local-helper-not-shared-header pattern
 * (see EVENT-013 audit row) while routing to the Phase-3 ABI. */
static int
register_native_method(struct UVM *vm, struct UObject *proto,
                       const char *name, urbi_native_method_fn fn)
{
    if (vm == NULL || proto == NULL || fn == NULL || name == NULL) {
        return URBI_ERR_INVALID_ARG;
    }

    UClosure *cl = urbi_native_closure_create(vm, fn);
    if (cl == NULL) return URBI_ERR_OOM;

    USymbol *sym = (USymbol *)ustr_intern(vm, name, urbi_strlen(name));
    if (sym == NULL) return URBI_ERR_OOM;

    UValue v = urbi_value_nil();
    v.kind = (uint8_t)UVAL_CLOSURE;
    v.v.p = (void *)cl;
    if (urbi_object_set_local_slot(vm, proto, sym, v) != 0) {
        return URBI_ERR_OOM;
    }
    return URBI_OK;
}

/* === Native method implementations (Phase-3 ABI) ===
 *
 * Signature: int fn(UVM *vm, UValue self, UValue *args, uint8_t nargs,
 *                   UValue *out).
 *   - `self` is the receiver from vm->last_recv (set by OP_GETSLOT).
 *   - `args` / `nargs` are the call-site arguments (no receiver in slot 0).
 *   - `*out` is the return value.
 *   - Returns UEXEC_OK on success, UEXEC_THROW on raise (urbi_raise_*). */

/* event_new_method: constructor — allocate a fresh UEvent.
 *
 * `self` is the Event proto; ignored.  Returns UVAL_EVENT wrapping the new
 * UEvent.  Raises OOM on alloc failure (EVENT-011 — route via the canonical
 * urbi_raise_oom helper rather than open-coded NIL return).
 *
 * Used by scripted `Event.new()` since Phase 7 (M6 stdlib).  Pre-Phase-7 the
 * legacy host-fn variant urbi_native_event_new returned NIL on OOM through a
 * different ABI; the Phase-3 ABI surfaces OOM as a throw. */
static int
event_new_method(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                 UValue *out)
{
    (void)self; (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Event.new", 0, nargs, out);

    UEvent *e = urbi_event_create(vm);
    if (e == NULL) return urbi_raise_oom(vm, out);

    *out = uvalue_from_event(e);
    return UEXEC_OK;
}

/* event_emit_method: async emit.
 *
 * `self` must be a UVAL_EVENT; args[0] is the optional payload.  Returns NIL.
 * Validates the receiver kind (EVENT-004) — non-event receivers raise rather
 * than dispatching c_event_emit_async on garbage. */
static int
event_emit_method(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                  UValue *out)
{
    if (nargs > 1) return urbi_raise_arity(vm, "Event.emit", 1, nargs, out);
    if (!uvalue_is_event(self))
        return urbi_raise_type(vm, "Event.emit: receiver must be an Event", out);

    UEvent *e = uvalue_as_event(self);
    c_event_emit_async(vm, e, event_optional_payload(nargs, args));

    *out = urbi_value_nil();
    return UEXEC_OK;
}

/* event_sync_emit_method: synchronous emit.  Same shape as event_emit_method
 * but routes through c_event_emit_sync. */
static int
event_sync_emit_method(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                       UValue *out)
{
    if (nargs > 1) return urbi_raise_arity(vm, "Event.syncEmit", 1, nargs, out);
    if (!uvalue_is_event(self))
        return urbi_raise_type(vm, "Event.syncEmit: receiver must be an Event", out);

    UEvent *e = uvalue_as_event(self);
    c_event_emit_sync(vm, e, event_optional_payload(nargs, args));

    *out = urbi_value_nil();
    return UEXEC_OK;
}

/* event_waituntil_method: block caller strand until event fires.
 *
 * `self` must be a UVAL_EVENT.  Returns the payload deposited by the emitting
 * strand on wake.  Routes through c_event_waituntil which handles the
 * scratch-context guard and parking semantics (spec #3 §6.4). */
static int
event_waituntil_method(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                       UValue *out)
{
    (void)args;
    if (nargs != 0)
        return urbi_raise_arity(vm, "Event.waituntil", 0, nargs, out);
    if (!uvalue_is_event(self))
        return urbi_raise_type(vm, "Event.waituntil: receiver must be an Event", out);

    UEvent *e = uvalue_as_event(self);
    *out = c_event_waituntil(vm, e);
    return UEXEC_OK;
}

/* === event_native_register ===
 *
 * Allocate vm->event_proto as a UObject in the URBI_ATOM_EVENT family,
 * then install the four native slots.  Called from urbi_vm_init after the
 * type-table setup and atom-proto walk are in place.
 * Returns UVM_OK on success, UVM_OOM if the proto object allocation fails.
 *
 * Phase 7 (M6 stdlib): vm->atom_event is set to the same object so that
 * urbi_object_atom(URBI_ATOM_EVENT) — used by the receiver-resolution path
 * for UVAL_EVENT (src/object/uobject_atom_dispatch.c) — finds the native
 * method slots.  Pre-Phase-7 vm->atom_event was lazy-allocated as a
 * separate empty proto, which silently routed `e.emit(...)` to a
 * proto-chain miss because the methods only lived on vm->event_proto.
 * Both fields point at one object now; the GC walker is idempotent on
 * double-shade. */
UVMError
event_native_register(struct UVM *vm)
{
    /* Allocate the event proto object.  Use URBI_ATOM_EVENT as the family
     * so IC entries and atom-family checks behave correctly.
     * urbi_object_alloc sets the root shape; proto chain is empty at birth.
     * The proto-chain hookup to root Object happens via the atom-singleton
     * path (urbi_object_atom would chain to root Object) — but since we
     * commandeer vm->atom_event below, do the same set_protos_single hookup
     * here so atom-dispatch sees a live root chain. */
    UObject *proto = urbi_object_alloc(vm, URBI_ATOM_EVENT);
    if (proto == NULL) {
        return UVM_OOM;
    }

    /* Chain the new proto onto root Object so OP_GETSLOT can walk past
     * Event.* into Object.* (clone, getSlot, setSlot, etc).  Mirrors the
     * urbi_object_atom path for non-root atoms. */
    UObject *root = urbi_object_root(vm);
    if (root == NULL) {
        return UVM_OOM;
    }
    urbi_object_set_protos_single(vm, proto, root);

    vm->event_proto = proto;
    vm->atom_event  = proto;  /* Phase 7: unify with atom-dispatch lookup. */

    /* EVENT-005: propagate slot-install failures.  An OOM during slot
     * intern/install would otherwise leave a partially populated event_proto
     * on the VM.  On any failure, reset event_proto to NULL (the proto cell
     * itself is GC-managed and will be collected at the next sweep) and
     * surface UVM_OOM to the caller.
     *
     * Phase 7 (M6 stdlib): slots are now UVAL_CLOSURE values with a
     * native_fn pointer (Phase-3 ABI), so OP_CALL can dispatch them directly
     * from script. */
    if (register_native_method(vm, proto, "new",       event_new_method)        != URBI_OK
     || register_native_method(vm, proto, "emit",      event_emit_method)       != URBI_OK
     || register_native_method(vm, proto, "syncEmit",  event_sync_emit_method)  != URBI_OK
     || register_native_method(vm, proto, "waituntil", event_waituntil_method)  != URBI_OK) {
        vm->event_proto = NULL;
        return UVM_OOM;
    }
    return UVM_OK;
}
