/* SPDX-License-Identifier: BSD-3-Clause */
/* object_root.c — M6 Phase 3 stdlib: Object root C-native methods.
 *
 * The nine root-level methods are the v1.0 surface for slot manipulation
 * + clone + proto graph mutation.  Each is registered as a UClosure with
 * native_fn pointing at the C body; the OP_CALL arm dispatches through
 * native_fn instead of pushing a bytecode frame when this field is set
 * (runtime/uclosure.h).
 *
 * Receiver routing: method-call sites are compiled as OP_SELF (loads
 * method + receiver into adjacent registers) followed by OP_CALL with
 * the method-flag bit set in C; the OP_CALL native arm reads `self`
 * from R[A+1] and passes it to the native function.  Plain
 * (non-method) calls pass nil as self.  (Pre-v1.6 the receiver came
 * from vm->last_recv, which got silently clobbered by intervening
 * OP_GETSLOTs in argument evaluation; the OP_SELF+method-flag scheme
 * eliminates that whole bug class — S42.)
 *
 * Phase 3 baseline error handling: urbi_raise_arity / _type / _oom /
 * _lookup print to stderr (when stderr is available; freestanding builds
 * silently drop) and return UEXEC_THROW to signal a fault to OP_CALL.
 * Wave 2 swaps these for proper Exception class wiring; the call sites
 * here keep a stable ABI so the swap is mechanical. */

#include "stdlib/object_root.h"

#include "gc/ugc.h"                /* UTYPE_CLOSURE */
#include "gc/ugc_incremental.h"    /* urbi_gc_alloc */
#include "chunk/umodule.h"        /* UValue, UVAL_*, UClosure typedef */
#include "object/uobject.h"        /* urbi_object_*, urbi_object_root */
#include "object/ushape.h"         /* urbi_shape_root */
#include "runtime/uclosure.h"      /* struct UClosure full def */
#include "runtime/umacros.h"       /* urbi_zero, urbi_strlen */
#include "sched/ustrand.h"         /* UEXEC_OK, UEXEC_THROW */
#include "urbi/types.h"            /* UErrCode, urbi_make_nil */
#include "urbi/urbi.h"             /* URBI_OK, URBI_ERR_OOM */
#include "value/uintern.h"         /* ustr_intern */
#include "vm/uvm.h"                /* UVM, urbi_vm_destroy */
#include "object/uic.h"            /* urbi_slot_get_slow */

#include <stdint.h>
#include <stddef.h>

/* Hosted-only stderr diagnostics; freestanding builds (e.g. cortex-m7 with
 * -ffreestanding) lack <stdio.h> and silently drop the messages. */
#if __STDC_HOSTED__
#  include <stdio.h>
#endif

/* === Forward declarations =================================================== */

static int obj_setSlot           (UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
static int obj_getSlot           (UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
static int obj_getSlotValue      (UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
static int obj_hasSlot           (UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
static int obj_removeSlot        (UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
static int obj_removeLocalSlot   (UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
static int obj_clone             (UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
static int obj_new               (UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
static int obj_addProto          (UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
static int obj_removeProto       (UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
static int obj_protos            (UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
static int obj_setProtos         (UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
static int obj_setProperty       (UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);
static int obj_protos_insertFront(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out);

/* === UValue helpers (zero-fill _pad bytes for bit-stable layout) =========== */

static UValue
uval_obj(UObject *o)
{
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_OBJECT;
    v.v.p = o;
    return v;
}

static UValue
uval_bool(int b)
{
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_BOOL;
    v.v.i = b ? 1 : 0;
    return v;
}

/* === urbi_native_closure_create ============================================ */

UClosure *
urbi_native_closure_create(UVM *vm, urbi_native_method_fn fn)
{
    if (vm == NULL || vm->alloc_fn == NULL || fn == NULL) return NULL;

    /* v0.8.4 Step C-3: native closures are GC-managed via urbi_gc_alloc,
     * same as bytecode closures (Step C-2).  The GC sweep + uclosure_destroy
     * finalizer reclaim them; no manual free or stdlib_closures threading. */
    size_t nbytes = sizeof(UClosure);
    UCell *c = urbi_gc_alloc(vm, nbytes, UTYPE_CLOSURE);
    if (c == NULL) return NULL;
    UClosure *cl = (UClosure *)c;

    /* gc_byte and type_tag are set by urbi_gc_alloc.  Proto = NULL means
     * uclosure_destroy skips uproto_root_of (no proto to dec-ref). */
    cl->proto      = NULL;
    cl->proto_inst = NULL;
    cl->nupvals    = 0;
    cl->native_fn  = fn;
    return cl;
}

/* === Native error helpers (Phase 3 baseline) =============================== */

int
urbi_raise_arity(UVM *vm, const char *fn_name, uint8_t expected,
                 uint8_t got, UValue *out)
{
    (void)vm;
    if (out != NULL) *out = urbi_make_nil();
#if __STDC_HOSTED__
    fprintf(stderr, "ArityError: %s expected %u args, got %u\n",
            (fn_name != NULL ? fn_name : "<unknown>"),
            (unsigned)expected, (unsigned)got);
#else
    (void)fn_name; (void)expected; (void)got;
#endif
    return UEXEC_THROW;
}

int
urbi_raise_type(UVM *vm, const char *msg, UValue *out)
{
    (void)vm;
    if (out != NULL) *out = urbi_make_nil();
#if __STDC_HOSTED__
    fprintf(stderr, "TypeError: %s\n", (msg != NULL ? msg : "<unspecified>"));
#else
    (void)msg;
#endif
    return UEXEC_THROW;
}

int
urbi_raise_oom(UVM *vm, UValue *out)
{
    (void)vm;
    if (out != NULL) *out = urbi_make_nil();
#if __STDC_HOSTED__
    fprintf(stderr, "OutOfMemoryError\n");
#endif
    return UEXEC_THROW;
}

int
urbi_raise_lookup(UVM *vm, USymbol *name, UValue *out)
{
    (void)vm; (void)name;
    if (out != NULL) *out = urbi_make_nil();
#if __STDC_HOSTED__
    fprintf(stderr, "LookupError: slot not found\n");
#endif
    return UEXEC_THROW;
}

/* === urbi_proto_list_create ================================================
 *
 * Phase 3 synthetic proto-list helper: returns a fresh UObject carrying a
 * `size` slot.  Wave 2 replaces this with a proper List atom (currently
 * the M6 stdlib roadmap row).  For Phase 3, fixtures that read
 * `obj.protos.size` find the field directly; a real iteration API isn't
 * shipped here. */

UObject *
urbi_proto_list_create(UVM *vm, UObject *recv)
{
    if (vm == NULL || recv == NULL) return NULL;

    UObject *list = urbi_object_alloc(vm, URBI_ATOM_LIST);
    if (list == NULL) return NULL;

    /* Install size = count(recv->protos). */
    USymbol *sym_size = (USymbol *)ustr_intern(vm, "size", 4);
    if (sym_size == NULL) return NULL;

    UValue n = urbi_make_nil();
    n.kind = (uint8_t)UVAL_INT;
    n.v.i = (int64_t)urbi_object_proto_count(recv);
    if (urbi_object_set_local_slot(vm, list, sym_size, n) != 0) return NULL;

    /* T63: thread the owner reference through so insertFront can mutate
     * the original receiver's prototype list.  Wave 2's List atom replaces
     * this synthetic with a proper list value; for Wave 1 an underscore-
     * prefixed hidden slot is sufficient. */
    USymbol *sym_owner = (USymbol *)ustr_intern(vm, "_owner", 6);
    if (sym_owner == NULL) return NULL;
    UValue owner = urbi_make_nil();
    owner.kind = (uint8_t)UVAL_OBJECT;
    /* Owner is a live UObject; insertFront on the synthetic list mutates
     * its proto chain in place via urbi_object_set_protos. */
    owner.v.p = recv;
    if (urbi_object_set_local_slot(vm, list, sym_owner, owner) != 0) return NULL;

    /* T63: install insertFront on this synthetic list.  Each protos call
     * allocates a fresh list; this attaches a per-list closure so the
     * lookup hits the synthetic before climbing to Object root.  Wave 1:
     * the per-list allocation cost is acceptable; Wave 2's List atom
     * installs insertFront once on the List atom proto. */
    UClosure *cl = urbi_native_closure_create(vm, obj_protos_insertFront);
    if (cl == NULL) return NULL;
    USymbol *sym_iF = (USymbol *)ustr_intern(vm, "insertFront", 11);
    if (sym_iF == NULL) return NULL;
    UValue clv = urbi_make_nil();
    clv.kind = (uint8_t)UVAL_CLOSURE;
    clv.v.p = cl;
    if (urbi_object_set_local_slot(vm, list, sym_iF, clv) != 0) return NULL;

    return list;
}

/* === Object.setSlot(name, value) =========================================== */

static int
obj_setSlot(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 2) return urbi_raise_arity(vm, "setSlot", 2, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "setSlot: self must be a UObject", out);
    if (args[0].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "setSlot: name must be a String", out);

    UObject *recv = (UObject *)self.v.p;
    USymbol *name = (USymbol *)args[0].v.p;
    int rc = urbi_object_set_local_slot(vm, recv, name, args[1]);
    if (rc != 0) return urbi_raise_oom(vm, out);

    *out = args[1];
    return UEXEC_OK;
}

/* === Object.getSlot(name) ================================================== */

static int
obj_getSlot(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "getSlot", 1, nargs, out);
    if (args[0].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "getSlot: name must be a String", out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "getSlot: self must be a UObject", out);

    UObject *recv = (UObject *)self.v.p;
    USymbol *name = (USymbol *)args[0].v.p;

    /* Use the unified slot-resolver (walks proto chain).  Returns 1 on
     * found, 0 on miss, -1 on error (depth-bound overflow). */
    UObject *holder = NULL;
    uint32_t slot_idx = 0;
    int rc = urbi_object_resolve_slot(vm, recv, name, &holder, &slot_idx);
    if (rc <= 0) return urbi_raise_lookup(vm, name, out);

    /* Read the slot value out of holder's flat USlot* array. */
    if (holder == NULL || holder->slots == NULL)
        return urbi_raise_lookup(vm, name, out);
    *out = holder->slots[slot_idx];
    return UEXEC_OK;
}

/* === Object.getSlotValue(name) ============================================
 *
 * T61: legacy alias for getSlot.  The 2014 inheritance.chk fixture uses
 * `getSlotValue("foo")` (line 17 in legacy/repos/aldebaran-urbi/tests/2.x/
 * inheritance.chk).  Same semantics — walk the prototype chain and return
 * the slot's value.  The legacy split between getSlot (returns the slot
 * descriptor) and getSlotValue (unwraps to the underlying value) doesn't
 * apply at v1.0 because USlot collapses onto UValue (pre-M4 design); both
 * names map to the same C body. */

static int
obj_getSlotValue(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    return obj_getSlot(vm, self, args, nargs, out);
}

/* === Object.hasSlot(name) ================================================== */

static int
obj_hasSlot(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "hasSlot", 1, nargs, out);
    if (args[0].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "hasSlot: name must be a String", out);
    UObject *recv;
    if (self.kind != (uint8_t)UVAL_OBJECT) {
        /* Atom receivers: the proto chain is the atom proto; the
         * underlying integer/etc. has no own slots, so route through the
         * atom proto for the hasSlot lookup. */
        recv = urbi_atom_proto_for_value(vm, self);
        if (recv == NULL) return urbi_raise_oom(vm, out);
    } else {
        recv = (UObject *)self.v.p;
    }
    const USymbol *name = (const USymbol *)args[0].v.p;
    UObject *holder = NULL;
    uint32_t slot_idx = 0;
    int rc = urbi_object_resolve_slot(vm, recv, name, &holder, &slot_idx);
    *out = uval_bool(rc == 1);
    return UEXEC_OK;
}

/* === Object.removeSlot(name) =============================================== */

static int
obj_removeSlot(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "removeSlot", 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "removeSlot: self must be a UObject", out);
    if (args[0].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm, "removeSlot: name must be a String", out);

    UObject *recv = (UObject *)self.v.p;
    const USymbol *name = (const USymbol *)args[0].v.p;
    int rc = urbi_object_remove_slot(vm, recv, name);
    if (rc != 0) return urbi_raise_oom(vm, out);

    /* Return self to allow chaining; legacy semantics returned nil but
     * that costs an extra arg parse on the script side.  Fixture
     * expectations follow the Phase 3 contract documented in the .chk
     * fixture. */
    *out = self;
    return UEXEC_OK;
}

/* === Object.removeLocalSlot(name) =========================================
 *
 * T62: legacy alias for removeSlot.  The 2014 inheritance.chk fixture uses
 * `removeLocalSlot("foo")` (line 36) to drop a slot installed on the
 * receiver itself (vs. removing from a proto chain).  At v1.0
 * urbi_object_remove_slot only operates on the receiver's own shape (no
 * walk into the proto chain), so the alias maps cleanly to removeSlot. */

static int
obj_removeLocalSlot(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    return obj_removeSlot(vm, self, args, nargs, out);
}

/* === Object.clone() ========================================================
 *
 * S-atom-clone-perf: atom receivers (UVAL_INT/_FLOAT/_BOOL/_STR/etc.)
 * return self directly with zero allocation.  UVAL_OBJECT receivers
 * route through urbi_object_clone (which already exists). */

static int
obj_clone(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "clone", 0, nargs, out);

    /* Atom short-circuit. */
    if (self.kind != (uint8_t)UVAL_OBJECT) {
        *out = self;
        return UEXEC_OK;
    }

    UObject *recv = (UObject *)self.v.p;
    UObject *clone = urbi_object_clone(vm, recv);
    if (clone == NULL) return urbi_raise_oom(vm, out);
    *out = uval_obj(clone);
    return UEXEC_OK;
}

/* === Object.new() ==========================================================
 *
 * T39 (spec §8): Foo.new() is the Class.new() idiom — clone Foo's proto
 * entry and return a fresh UObject.  At v1.0 .new() is identical to
 * .clone(); the distinction is reserved for future per-class init-hook
 * semantics (urbiscript has no special init() idiom — user code calls
 * obj.init() explicitly after .new() if needed).
 *
 * Atom short-circuit (S-atom-clone-perf) applies via obj_clone delegation:
 * 1.new() returns 1 directly with zero allocation. */

static int
obj_new(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 0) return urbi_raise_arity(vm, "new", 0, nargs, out);
    /* Delegate to obj_clone so the atom short-circuit + UObject path stay
     * in one place.  When the language gains init hooks, this delegation
     * becomes the call site for the post-clone init dispatch. */
    return obj_clone(vm, self, args, nargs, out);
}

/* === Object.addProto(parent) =============================================== */

static int
obj_addProto(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "addProto", 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "addProto: self must be a UObject", out);
    if (args[0].kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "addProto: argument must be a UObject", out);

    UObject *recv = (UObject *)self.v.p;
    UObject *proto = (UObject *)args[0].v.p;
    int rc = urbi_object_add_proto(vm, recv, proto);
    if (rc == URBI_ERR_INVALID_ARG) {
        /* Either valid_proto rejected (atom-family mismatch / cycle) or
         * the proto-list cap is exceeded.  Both surface as TypeError to
         * scripted callers. */
        return urbi_raise_type(vm,
            "addProto: invalid prototype (atom-family mismatch, cycle, or cap)", out);
    }
    if (rc != URBI_OK) return urbi_raise_oom(vm, out);

    *out = self;
    return UEXEC_OK;
}

/* === Object.removeProto(parent) ============================================ */

static int
obj_removeProto(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "removeProto", 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "removeProto: self must be a UObject", out);
    if (args[0].kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "removeProto: argument must be a UObject", out);

    UObject *recv = (UObject *)self.v.p;
    const UObject *proto = (const UObject *)args[0].v.p;
    /* Idempotent per legacy semantics — silent no-op if proto wasn't
     * present.  urbi_object_remove_proto already implements that. */
    (void)urbi_object_remove_proto(vm, recv, proto);

    *out = self;
    return UEXEC_OK;
}

/* === Object.protos ========================================================= */

static int
obj_protos(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "protos", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "protos: self must be a UObject", out);

    UObject *recv = (UObject *)self.v.p;
    UObject *list = urbi_proto_list_create(vm, recv);
    if (list == NULL) return urbi_raise_oom(vm, out);
    *out = uval_obj(list);
    return UEXEC_OK;
}

/* === Object.setProtos(parent) ============================================== */

static int
obj_setProtos(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "setProtos", 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "setProtos: self must be a UObject", out);
    /* Wave 1 limited: List literal lex defers to Wave 2.  Accept a single
     * UObject and treat as a one-element list.  A future setProtos that
     * accepts a List value will branch on args[0].kind == UVAL_LIST. */
    if (args[0].kind != (uint8_t)UVAL_OBJECT) {
        return urbi_raise_type(vm,
            "setProtos: List literal deferred to Wave 2; pass a single UObject", out);
    }

    UObject *recv = (UObject *)self.v.p;
    UObject *single[1];
    single[0] = (UObject *)args[0].v.p;
    int rc = urbi_object_set_protos(vm, recv, single, 1);
    if (rc == URBI_ERR_INVALID_ARG)
        return urbi_raise_type(vm, "setProtos: invalid prototype", out);
    if (rc != URBI_OK) return urbi_raise_oom(vm, out);

    *out = self;
    return UEXEC_OK;
}

/* === protos.insertFront(proto) ===========================================
 *
 * T63: Wave-1 stub for the legacy `C.protos.insertFront(A)` idiom (used in
 * legacy/repos/aldebaran-urbi/tests/2.x/shared-protos.chk line 12).
 *
 * `self` is the synthetic UObject returned from .protos; we extract the
 * underlying owner via the hidden `_owner` slot, prepend args[0] to the
 * owner's prototype list, and rebuild via urbi_object_set_protos.
 *
 * Wave 2 retires this when the proper List atom replaces the synthetic
 * proto-list (the List atom installs insertFront once on the List atom
 * proto and operates on the underlying list storage).  For Wave 1 the
 * mutation flows through urbi_object_set_protos with a stack-bounded
 * UObject* array.  If the combined list exceeds the proto cap,
 * set_protos returns URBI_ERR_INVALID_ARG which surfaces as TypeError. */

#ifndef URBI_PROTO_LIST_INSERT_FRONT_CAP
/* Cap for stack-allocated combined-protos array.  Mirrors the proto-cap
 * inside urbi_object_set_protos (URBI_PROTOS_SETPROTOS_CAP, 64).  Sized
 * 65 here so the worst case (existing chain of length 64 + 1 prepend) is
 * handed to set_protos which then rejects with URBI_ERR_INVALID_ARG. */
#define URBI_PROTO_LIST_INSERT_FRONT_CAP 65
#endif

static int
obj_protos_insertFront(UVM *vm, UValue self, UValue *args, uint8_t nargs,
                       UValue *out)
{
    if (nargs != 1) return urbi_raise_arity(vm, "insertFront", 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm,
            "insertFront: self must be the synthetic protos UObject", out);
    if (args[0].kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm,
            "insertFront: argument must be a UObject", out);

    UObject *list = (UObject *)self.v.p;
    const USymbol *sym_owner = (const USymbol *)ustr_intern(vm, "_owner", 6);
    if (sym_owner == NULL) return urbi_raise_oom(vm, out);

    UObject *holder = NULL;
    uint32_t idx = 0U;
    int rc = urbi_object_resolve_slot(vm, list, sym_owner, &holder, &idx);
    if (rc != 1 || holder == NULL || holder->slots == NULL)
        return urbi_raise_type(vm,
            "insertFront: synthetic protos list is missing _owner", out);

    UValue owner_v = holder->slots[idx];
    if (owner_v.kind != (uint8_t)UVAL_OBJECT || owner_v.v.p == NULL)
        return urbi_raise_type(vm,
            "insertFront: synthetic protos _owner is not a UObject", out);

    UObject *owner = (UObject *)owner_v.v.p;
    UObject *prepend = (UObject *)args[0].v.p;

    uint32_t old_n = urbi_object_proto_count(owner);
    if (old_n + 1U > URBI_PROTO_LIST_INSERT_FRONT_CAP)
        return urbi_raise_type(vm, "insertFront: proto list cap exceeded", out);

    UObject *combined[URBI_PROTO_LIST_INSERT_FRONT_CAP];
    combined[0] = prepend;
    for (uint32_t i = 0U; i < old_n; i++) {
        combined[i + 1U] = urbi_object_proto_at(owner, i);
    }

    int src = urbi_object_set_protos(vm, owner, combined, old_n + 1U);
    if (src == URBI_ERR_INVALID_ARG)
        return urbi_raise_type(vm,
            "insertFront: invalid prototype (atom-family mismatch / cycle / cap)", out);
    if (src != URBI_OK) return urbi_raise_oom(vm, out);

    /* Refresh the synthetic list's `size` slot so `protos.size` reflects
     * the new count for any subsequent reads.  (Each .protos call returns
     * a fresh synthetic, but the caller may chain off this same list.) */
    USymbol *sym_size = (USymbol *)ustr_intern(vm, "size", 4);
    if (sym_size != NULL) {
        UValue nval = urbi_make_nil();
        nval.kind = (uint8_t)UVAL_INT;
        nval.v.i = (int64_t)urbi_object_proto_count(owner);
        (void)urbi_object_set_local_slot(vm, list, sym_size, nval);
    }

    *out = self;
    return UEXEC_OK;
}

/* === Object.setProperty(name, prop_name, value) ============================
 *
 * T41 (M6 Wave 2) — backing for the `get`/`set` parse sugar.  Installs a
 * slot property on the receiver:
 *
 *   prop_name == "oget" → install URBI_SLOT_FLAG_OGET (slot read calls value)
 *   prop_name == "oset" → install URBI_SLOT_FLAG_OSET (slot write calls value)
 *   prop_name == "constant" → install URBI_SLOT_FLAG_CONSTANT (write-protect)
 *
 * `urbi_object_install_property` requires the slot to pre-exist; if the
 * slot is missing we materialize it with a nil placeholder before
 * installing the property bit.  Legacy semantics treat the `get`/`set`
 * sugar as creating-and-installing in one shot. */

static int
obj_setProperty(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    if (nargs != 3) return urbi_raise_arity(vm, "setProperty", 3, nargs, out);
    if (self.kind != (uint8_t)UVAL_OBJECT)
        return urbi_raise_type(vm, "setProperty: self must be a UObject", out);
    if (args[0].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm,
            "setProperty: name must be a String", out);
    if (args[1].kind != (uint8_t)UVAL_STR)
        return urbi_raise_type(vm,
            "setProperty: prop name must be a String", out);

    UObject *recv = (UObject *)self.v.p;
    USymbol *name = (USymbol *)args[0].v.p;
    const USymbol *prop = (const USymbol *)args[1].v.p;

    /* Map the prop name to a flag bit.  Compare against the interned
     * USymbols' bytes — USymbol is opaque here but its layout exposes the
     * leading length-prefixed bytes via ustr_intern's NUL-terminated view.
     * We re-intern the constant strings to compare by pointer identity. */
    const USymbol *sym_oget     = (const USymbol *)ustr_intern(vm, "oget", 4);
    const USymbol *sym_oset     = (const USymbol *)ustr_intern(vm, "oset", 4);
    const USymbol *sym_constant = (const USymbol *)ustr_intern(vm, "constant", 8);
    if (sym_oget == NULL || sym_oset == NULL || sym_constant == NULL)
        return urbi_raise_oom(vm, out);

    uint8_t flag_bit;
    if (prop == sym_oget) {
        flag_bit = URBI_SLOT_FLAG_OGET;
    } else if (prop == sym_oset) {
        flag_bit = URBI_SLOT_FLAG_OSET;
    } else if (prop == sym_constant) {
        flag_bit = URBI_SLOT_FLAG_CONSTANT;
    } else {
        return urbi_raise_type(vm,
            "setProperty: prop name must be one of \"oget\", \"oset\", \"constant\"",
            out);
    }

    /* T41 critical fix: validate the closure's arity at install time.
     * The runtime dispatch path (urbi_run_closure_on_scratch) does not
     * check nparams against the call shape — getter dispatch passes 0
     * args, setter dispatch passes 1 arg.  A wrong-arity closure body
     * would read uninitialized register slots → SIGSEGV.  Reject here
     * with a clear ArityError instead.  `constant` carries through
     * whatever value is passed (no closure expected), so no check.
     *
     * args[2] is a UClosure for oget/oset (emit_property_decl_arm
     * synthesizes an AST_FUNCTION arg).  Nparams lives on the proto. */
    if (flag_bit == URBI_SLOT_FLAG_OGET || flag_bit == URBI_SLOT_FLAG_OSET) {
        if (args[2].kind != (uint8_t)UVAL_CLOSURE || args[2].v.p == NULL) {
            return urbi_raise_type(vm,
                "setProperty: oget/oset requires a function value", out);
        }
        const struct UClosure *cl = (const struct UClosure *)args[2].v.p;
        const uint8_t expected = (flag_bit == URBI_SLOT_FLAG_OGET) ? 0U : 1U;
        const uint8_t got = (cl->proto != NULL) ? cl->proto->nparams : 0U;
        if (got != expected) {
            const char *fn_name =
                (flag_bit == URBI_SLOT_FLAG_OGET) ? "get" : "set";
            return urbi_raise_arity(vm, fn_name, expected, got, out);
        }
    }

    /* Materialize the slot with a nil placeholder if it doesn't exist —
     * legacy `get x()` / `set x(v)` sugar implicitly creates the slot. */
    UObject *holder = NULL;
    uint32_t idx = 0U;
    int rc_resolve = urbi_object_resolve_slot(vm, recv, name, &holder, &idx);
    if (rc_resolve != 1 || holder != recv) {
        /* Slot missing on the receiver's local shape (or only present on
         * a proto).  Install a nil placeholder on the receiver so the
         * subsequent install_property call finds the slot. */
        UValue placeholder = urbi_make_nil();
        int rc_set = urbi_object_set_local_slot(vm, recv, name, placeholder);
        if (rc_set != 0) return urbi_raise_oom(vm, out);
    }

    int rc = urbi_object_install_property(vm, recv, name, flag_bit, args[2]);
    if (rc == URBI_ERR_INVALID_ARG)
        return urbi_raise_type(vm,
            "setProperty: invalid argument to install_property", out);
    if (rc == URBI_ERR_SLOT_NOT_FOUND)
        return urbi_raise_lookup(vm, (USymbol *)name, out);
    if (rc != URBI_OK) return urbi_raise_oom(vm, out);

    *out = self;
    return UEXEC_OK;
}

/* === urbi_object_root_register ============================================= */

typedef struct {
    const char           *name;
    urbi_native_method_fn fn;
} ObjectMethodEntry;

static const ObjectMethodEntry OBJECT_METHODS[] = {
    { "setSlot",         obj_setSlot         },
    { "getSlot",         obj_getSlot         },
    { "getSlotValue",    obj_getSlotValue    },   /* T61: legacy alias for getSlot */
    { "hasSlot",         obj_hasSlot         },
    { "removeSlot",      obj_removeSlot      },
    { "removeLocalSlot", obj_removeLocalSlot },   /* T62: legacy alias for removeSlot */
    { "clone",           obj_clone           },
    { "new",             obj_new             },
    { "addProto",        obj_addProto        },
    { "removeProto",     obj_removeProto     },
    { "protos",          obj_protos          },
    { "setProtos",       obj_setProtos       },
    { "setProperty",     obj_setProperty     }    /* T41: backs get/set sugar */
};

#define OBJECT_METHODS_COUNT (sizeof(OBJECT_METHODS) / sizeof(OBJECT_METHODS[0]))

int
urbi_object_root_register(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;
    UObject *root = urbi_object_root(vm);
    if (root == NULL) return URBI_ERR_OOM;

    size_t i;
    for (i = 0; i < OBJECT_METHODS_COUNT; i++) {
        UClosure *cl = urbi_native_closure_create(vm, OBJECT_METHODS[i].fn);
        if (cl == NULL) return URBI_ERR_OOM;

        USymbol *sym = (USymbol *)ustr_intern(
            vm, OBJECT_METHODS[i].name,
            urbi_strlen(OBJECT_METHODS[i].name));
        if (sym == NULL) return URBI_ERR_OOM;

        UValue v = urbi_make_nil();
        v.kind = (uint8_t)UVAL_CLOSURE;
        v.v.p = cl;
        int rc = urbi_object_set_local_slot(vm, root, sym, v);
        if (rc != 0) return URBI_ERR_OOM;
    }
    return URBI_OK;
}
