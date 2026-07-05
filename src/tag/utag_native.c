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
 * W4/v0.10.2: Tag scripted surface (reactive audit F3, audit-1 F5).
 *   tag_new_native:   Tag.new(name) — allocate + name + realm-parent.
 *   tag_stop_native:  tag.stop()    — forward to urbi_tag_stop.
 *   tag_freeze_native:   tag.freeze()   — set UTAG_FLAG_FROZEN.
 *   tag_unfreeze_native: tag.unfreeze() — clear UTAG_FLAG_FROZEN.
 *   tag_block_native:    tag.block()    — future; raises NotImplemented at v1.0.
 *   tag_unblock_native:  tag.unblock()  — future; raises NotImplemented at v1.0.
 *   tag_enter_native:    tag.enter — return lazy enter_event.
 *   tag_leave_native:    tag.leave — return lazy leave_event.
 *
 * Native getter binding on vm->tag_proto:
 *   W4 promotes vm->tag_proto to a UVAL_CLOSURE-slot proto so OP_CALL
 *   can dispatch scripted tag.stop() etc.  vm->atom_tag is unified with
 *   vm->tag_proto (same UObject) so urbi_atom_proto_for_value(UVAL_TAG)
 *   finds the native methods.
 *
 * tag_enter_leave_setter_protected:
 *   Any write to tag.enter or tag.leave raises URBI_ERR_PROTECTED_SLOT.
 *   A UVAL_HOST_FN setter stub is installed on the `_enter_set`/`_leave_set`
 *   slot names; reachable today from C-level tests via direct slot lookup. */

#include "tag/utag_native.h"

#include "vm/uvm.h"
#include "tag/utag.h"              /* UTag, tag->enter_event / leave_event */
/* urbi_tag_create + urbi_tag_stop declared in include/urbi/urbi.h (W4) */
#include "event/uevent.h"            /* UEvent, urbi_event_create */
#include "event/uevent_native.h"      /* uvalue_from_event */
#include "value/uintern.h"           /* ustr_intern */
#include "object/uobject.h"    /* urbi_object_alloc, urbi_object_set_protos_single */
#include "urbi/object.h"       /* urbi_object_root (W4) */
#include "runtime/umacros.h"   /* URBI_INTERNAL_ASSERT (TAGCH-002), urbi_strlen, urbi_zero */
#include "runtime/uclosure.h"  /* UClosure, urbi_native_method_fn (W4) */
#include "urbi/urbi.h"         /* URBI_ERR_PROTECTED_SLOT, URBI_ERR_OOM, UHostFn,
                                   urbi_tag_stop, urbi_tag_create */
#include "urbi/types.h"        /* urbi_make_tag, urbi_make_nil (W4) */
#include "realm/urealm.h"      /* URealm, realm->tag (W4) */
#include "stdlib/object_root.h" /* urbi_native_closure_create, urbi_raise_* (W4) */
/* urbi_gc_slot_pre_store (Dijkstra forward barrier; barrier-only variant since
 * tag->enter_event / tag->leave_event are UEvent*, not UValue*, so the
 * combined urbi_gc_slot_store cannot be used here) is reached via urbi/gc.h
 * pulled in by vm/uvm.h above. */
#include "sched/ustrand.h"           /* UStrand (for URBI_ERR_* throw helpers) */
#include "runtime/ucleanup.h"        /* UCleanupEntry, UCLEANUP_TAG_SCOPE (W2 v0.10.9) */
#include "stdlib/temporal.h"         /* urbi_tag_owns_periodic (B5/SCHED-N2) */
#include <stdbool.h>
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
        urbi_throw(vm, vm->cur_strand, err);
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
    urbi_throw(s->vm, s, err);
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

/* === register_native_method (W4) ===
 *
 * Install a UVAL_CLOSURE slot named `name` on `proto` with native_fn = fn.
 * OP_CALL dispatches through native_fn directly (Phase-3 ABI).
 * Returns URBI_OK on success, URBI_ERR_OOM on alloc/intern failure. */
static int
register_native_method_tag(struct UVM *vm, struct UObject *proto,
                           const char *name, urbi_native_method_fn fn)
{
    if (vm == NULL || proto == NULL || fn == NULL || name == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    UClosure *cl = urbi_native_closure_create(vm, fn);
    if (cl == NULL) return URBI_ERR_OOM;

    USymbol *sym = (USymbol *)ustr_intern(vm, name, urbi_strlen(name));
    if (sym == NULL) return URBI_ERR_OOM;

    UValue v;
    urbi_zero(&v, sizeof(v));
    v.kind = (uint8_t)UVAL_CLOSURE;
    v.v.p  = (void *)cl;
    if (urbi_object_set_local_slot(vm, proto, sym, v) != 0) {
        return URBI_ERR_OOM;
    }
    return URBI_OK;
}

/* === W4/v0.10.2 native method implementations (Phase-3 ABI) ===
 *
 * All methods: int fn(UVM *vm, UValue self, UValue *args, uint8_t nargs,
 *                     UValue *out).
 * self: for method calls (preceded by OP_SELF), this is the UVAL_TAG value.
 * Return UEXEC_OK on success, UEXEC_THROW on error. */

/* tag_new_native: Tag.new() / Tag.new(name) — allocate a UTag + name + parent.
 *
 * self: the Tag proto (ignored — acts as a factory).
 * args[0]: optional string name for the new tag (legacy uses Tag.new with 0 args).
 * Returns UVAL_TAG wrapping the new UTag.
 *
 * 0 args: anonymous tag (no name stored).
 * 1 arg:  string name interned into tag->name.
 *
 * Uses the cur_strand's realm so the tag lives in the right scope.
 * Falls back to the VM's global realm if cur_strand->realm is NULL. */
static int
tag_new_native(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
               UValue *out)
{
    (void)self;
    if (nargs > 1) return urbi_raise_arity(vm, "Tag.new", 1, nargs, out);

    const char *name_cstr = NULL;
    size_t name_len = 0;
    if (nargs == 1) {
        if (args[0].kind != (uint8_t)UVAL_STR)
            return urbi_raise_type(vm, "Tag.new: argument must be a String name", out);
        name_cstr = (const char *)args[0].v.p;
        name_len = urbi_strlen(name_cstr);
    }

    /* Determine the realm from the dispatching strand. */
    struct URealm *r = NULL;
    if (vm->cur_strand != NULL && vm->cur_strand->realm != NULL) {
        r = vm->cur_strand->realm;
    } else {
        r = urbi_realm_global(vm);
    }
    if (r == NULL) return urbi_raise_oom(vm, out);

    UTag *t = urbi_tag_create(vm, r, name_cstr, name_len);
    if (t == NULL) return urbi_raise_oom(vm, out);

    *out = urbi_make_tag(t);
    return UEXEC_OK;
}

/* W2 v0.10.9 helper for D3 fatal outside-scope check.
 *
 * Walk the calling strand's cleanup stack to find a TAG_SCOPE entry whose
 * owning_tag matches t.  Returns true if found.  Used by the D3
 * outside-scope check in tag_stop_native below. */
static bool
strand_has_tag_in_scope(const struct UStrand *s, const UTag *t)
{
    if (s == NULL || t == NULL) return false;
    for (uint16_t i = 0; i < s->cleanup_depth; i++) {
        const UCleanupEntry *e = &s->cleanup_base[i];
        if (e->kind == (uint8_t)UCLEANUP_TAG_SCOPE && e->owning_tag == t) {
            return true;
        }
    }
    return false;
}

/* tag_stop_native: tag.stop() / tag.stop(value) — deposit TAG_STOP on all
 * member strands.
 *
 * self must be UVAL_TAG.  Forwards to urbi_tag_stop with the optional
 * stop-value (nil if no argument).  Returns nil.
 *
 * v0.10.9 W1: valued-stop arity relax — accepts 0 or 1 argument; args[0]
 * is plumbed through as the unwind_value (S5 valued-stop ratification).
 *
 * v0.10.9 W2: D3 fatal outside-scope check — if there are no member
 * strands AND the calling strand has no TAG_SCOPE for the receiver in
 * its cleanup stack, deposit a local TAG_STOP via urbi_tag_stop_local so
 * the unwind walker (uunwind.c:304 "empty cleanup stack → fatal escalation")
 * escalates.  Closes design-risks v0.10.7-D (D3 ratified 2026-05-27). */
static int
tag_stop_native(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                UValue *out)
{
    if (nargs > 1) return urbi_raise_arity(vm, "Tag.stop", 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_TAG)
        return urbi_raise_type(vm, "Tag.stop: self must be a Tag", out);

    UTag *t = (UTag *)self.v.p;
    if (t == NULL) return urbi_raise_type(vm, "Tag.stop: NULL tag pointer", out);

    UValue stop_value = (nargs == 1) ? args[0] : urbi_make_nil();

    /* B5/SCHED-N2 (2026-07-04): snapshot whether the tag owns a live periodic
     * BEFORE calling urbi_tag_stop.  urbi_tag_stop now calls
     * urbi_periodics_stop_owned_by which sets unregister_pending=1, so a
     * post-stop call to urbi_tag_owns_periodic would return false even for a tag
     * that DID own a live periodic.  Snapshotting here preserves the pre-stop
     * "this tag has a reason to exist" verdict for the D3 guard below.
     * A second stop on the same tag — after the first cancelled its periodic —
     * intentionally falls through to the no-active-scope fatal, same as
     * stopping a never-armed tag.
     * v0.13.5 (v0.13.4-A): similarly snapshot had_watcher BEFORE urbi_tag_stop
     * because urbi_tag_stop cascades watchers out of member_watchers_head. */
    bool had_periodic = urbi_tag_owns_periodic(vm, t);
    bool had_watcher  = (t->member_watchers_head != NULL);

    /* Cross-strand deposit on member strands (existing path). */
    urbi_tag_stop(vm, t, stop_value);

    /* D3 (workspace-root compatibility-decisions ledger §S5c, design-risks
     * v0.10.7-D ratified 2026-05-27): if neither the calling strand nor any
     * other strand has this tag in their cleanup stack, deposit TAG_STOP
     * locally so the unwind walker escalates to fatal.  Cross-strand
     * deposit above handled the "other strands" half; the local deposit
     * covers the case where member_strands_head was empty AND we're not
     * in t's scope.  If we ARE in t's scope, urbi_tag_stop already
     * deposited on us via member_strands_head; no local deposit needed.
     *
     * Surface the error in vm->last_error / last_errmsg so REPL recovery
     * (urbi_repl_eval at src/chunk/uchunk_strand.c) doesn't silently
     * convert the fatal-escalated strand to nil — script-level unhandled
     * `throw` keeps that recovery contract; D3 outside-scope tag.stop()
     * surfaces as a visible runtime error per the ratified semantics.
     *
     * B5/SCHED-N2: !had_periodic guards the case where the tag owns a live
     * periodic (e.g. `t: every(P) body()`) — a legitimate stop target even when
     * member_strands_head is empty (body strand completed its last fire).
     * v0.13.5 (v0.13.4-A): !had_watcher guards `t: at (cond) body` — the
     * user-owned tag's scope closes immediately but the watcher persists until
     * t.stop(); stopping it is legitimate (it IS the stop target). */
    if (t->member_strands_head == NULL &&
        !strand_has_tag_in_scope(vm->cur_strand, t) &&
        !had_periodic &&
        !had_watcher) {
        vm->last_error = UVM_TYPE_ERROR;
        urbi_strncpy_truncating(vm->last_errmsg, UVM_ERRMSG_CAP,
            "tag.stop with no active scope");
        urbi_tag_stop_local(vm, vm->cur_strand, t, stop_value);
    }

    *out = urbi_make_nil();
    return UEXEC_OK;
}

/* tag_freeze_native: tag.freeze() — cross-strand SUSPENDED via FREEZE.
 *
 * Forwards to urbi_tag_freeze (W3c).  Sets UTAG_FLAG_FROZEN and suspends
 * every member strand with USTRAND_REASON_FREEZE.  Replaces the flag-only
 * stub from v0.10.2 W4.  Returns nil. */
static int
tag_freeze_native(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                  UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Tag.freeze", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_TAG)
        return urbi_raise_type(vm, "Tag.freeze: self must be a Tag", out);

    UTag *t = (UTag *)self.v.p;
    if (t == NULL) return urbi_raise_type(vm, "Tag.freeze: NULL tag pointer", out);

    urbi_tag_freeze(vm, t);
    *out = urbi_make_nil();
    return UEXEC_OK;
}

/* tag_unfreeze_native: tag.unfreeze() — symmetric to tag.freeze().
 * Clears UTAG_FLAG_FROZEN and resumes FREEZE-suspended members. */
static int
tag_unfreeze_native(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                    UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Tag.unfreeze", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_TAG)
        return urbi_raise_type(vm, "Tag.unfreeze: self must be a Tag", out);

    UTag *t = (UTag *)self.v.p;
    if (t == NULL) return urbi_raise_type(vm, "Tag.unfreeze: NULL tag pointer", out);

    urbi_tag_unfreeze(vm, t);
    *out = urbi_make_nil();
    return UEXEC_OK;
}

/* tag_block_native: tag.block() / tag.block(value) — cross-strand suspend.
 *
 * Forwards to urbi_tag_block (W3b).  0-arg form uses nil as the resume value;
 * 1-arg form passes the supplied value.  Sets UTAG_FLAG_BLOCKED on the tag
 * and suspends every member strand.  Returns nil.
 *
 * Script-side delivery of resume_value into the strand's result register
 * (valued-block) is deferred to v1.x (W3f); the C API already accepts the
 * value so callers don't need a re-API later. */
static int
tag_block_native(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                 UValue *out)
{
    if (nargs > 1) return urbi_raise_arity(vm, "Tag.block", 1, nargs, out);
    if (self.kind != (uint8_t)UVAL_TAG)
        return urbi_raise_type(vm, "Tag.block: self must be a Tag", out);

    UTag *t = (UTag *)self.v.p;
    if (t == NULL) return urbi_raise_type(vm, "Tag.block: NULL tag pointer", out);

    UValue resume_value = (nargs == 1) ? args[0] : urbi_make_nil();
    urbi_tag_block(vm, t, resume_value);
    *out = urbi_make_nil();
    return UEXEC_OK;
}

/* tag_unblock_native: tag.unblock() — symmetric to tag.block().
 * Clears UTAG_FLAG_BLOCKED and resumes BLOCK-suspended members. */
static int
tag_unblock_native(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                   UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Tag.unblock", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_TAG)
        return urbi_raise_type(vm, "Tag.unblock: self must be a Tag", out);

    UTag *t = (UTag *)self.v.p;
    if (t == NULL) return urbi_raise_type(vm, "Tag.unblock: NULL tag pointer", out);

    urbi_tag_unblock(vm, t);
    *out = urbi_make_nil();
    return UEXEC_OK;
}

/* W3d (v0.10.9): tag.frozen() and tag.blocked() — 0-arg method-style getters.
 *
 * Read UTAG_FLAG_FROZEN / UTAG_FLAG_BLOCKED bits and return a Boolean.
 * Full property-style dispatch (so `tag.frozen` without parens reads as a
 * property) is OPROPS-deferred to v1.x per TAGCH-013 — at v1.0 these are
 * 0-arg method calls.  Returns urbi_make_bool(...).
 *
 * Mirrors the urbi_make_bool helper installed by the boolean stdlib. */

static UValue
tag_make_bool(bool b)
{
    /* Inline minimal Bool — boolean stdlib's urbi_make_bool lives elsewhere
     * and pulling it into utag_native.c risks circular includes.  The atom
     * dispatch path treats UVAL_BOOL with v.i ∈ {0,1} as the canonical
     * Boolean encoding. */
    UValue v;
    v.kind = (uint8_t)UVAL_BOOL;
    v.v.i  = b ? 1 : 0;
    return v;
}

static int
tag_frozen_native(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                  UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Tag.frozen", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_TAG)
        return urbi_raise_type(vm, "Tag.frozen: self must be a Tag", out);

    UTag *t = (UTag *)self.v.p;
    if (t == NULL) return urbi_raise_type(vm, "Tag.frozen: NULL tag pointer", out);

    *out = tag_make_bool((t->flags & UTAG_FLAG_FROZEN) != 0U);
    return UEXEC_OK;
}

static int
tag_blocked_native(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                   UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Tag.blocked", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_TAG)
        return urbi_raise_type(vm, "Tag.blocked: self must be a Tag", out);

    UTag *t = (UTag *)self.v.p;
    if (t == NULL) return urbi_raise_type(vm, "Tag.blocked: NULL tag pointer", out);

    *out = tag_make_bool((t->flags & UTAG_FLAG_BLOCKED) != 0U);
    return UEXEC_OK;
}

/* tag_enter_native: tag.enter — lazy-allocate enter_event and return it.
 * Used by `at (t.enter?)` watcher installs from script. */
static int
tag_enter_native(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                 UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Tag.enter", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_TAG)
        return urbi_raise_type(vm, "Tag.enter: self must be a Tag", out);

    UTag *t = (UTag *)self.v.p;
    if (t == NULL) return urbi_raise_type(vm, "Tag.enter: NULL tag pointer", out);

    *out = tag_enter_getter(vm, t);
    return UEXEC_OK;
}

/* tag_leave_native: tag.leave — lazy-allocate leave_event and return it.
 * Used by `at (t.leave?)` watcher installs from script. */
static int
tag_leave_native(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                 UValue *out)
{
    (void)args;
    if (nargs != 0) return urbi_raise_arity(vm, "Tag.leave", 0, nargs, out);
    if (self.kind != (uint8_t)UVAL_TAG)
        return urbi_raise_type(vm, "Tag.leave: self must be a Tag", out);

    UTag *t = (UTag *)self.v.p;
    if (t == NULL) return urbi_raise_type(vm, "Tag.leave: NULL tag pointer", out);

    *out = tag_leave_getter(vm, t);
    return UEXEC_OK;
}

/* === tag_native_register === */

UVMError
tag_native_register(struct UVM *vm)
{
    /* TAGCH-016: drives urbi_object_alloc + slot installs, neither of which
     * is ISR-safe.  Mirror src/changed/uchanged.c:32. */
    URBI_ASSERT_NOT_ISR(vm);

    /* GC soundness (v0.13.2): resolve root Object BEFORE allocating the
     * proto and publish vm->tag_proto IMMEDIATELY after allocation — same
     * unrooted-C-local window as event_native_register (see the comment
     * there). */
    UObject *root = urbi_object_root(vm);
    if (root == NULL) {
        return UVM_OOM;
    }

    UObject *proto = urbi_object_alloc(vm, URBI_ATOM_TAG);
    if (proto == NULL) {
        return UVM_OOM;   /* OOM: leave tag_proto NULL */
    }
    vm->tag_proto = proto;

    /* Chain the tag proto onto root Object so OP_GETSLOT can walk past
     * Tag.* into Object.* (clone, getSlot, setSlot, etc.).  Mirrors the
     * same hookup done by event_native_register for Event.  No allocation
     * between the proto alloc and this call. */
    urbi_object_set_protos_single(vm, proto, root);

    /* W4/v0.10.2: unify vm->atom_tag with vm->tag_proto so
     * urbi_atom_proto_for_value(UVAL_TAG) finds the native method slots via
     * urbi_object_atom(vm, URBI_ATOM_TAG).  Mirrors the event_native_register
     * pattern: vm->atom_event = vm->event_proto = proto. */
    vm->atom_tag = proto;

    /* TAGCH-004: propagate slot-install failures.  Chain with || and on any
     * non-zero return clear vm->tag_proto / vm->atom_tag and surface UVM_OOM.
     * The proto cell stays GC-managed and is collected at the next sweep.
     *
     * W4 adds scripted Tag.new / .stop / .freeze / .unfreeze / .block /
     * .unblock / .enter / .leave as UVAL_CLOSURE native methods.
     * The `_enter_set` / `_leave_set` host-fn stubs stay for C-test
     * compatibility until tag-property UProps dispatch lands (v1.x). */
    if (register_host_fn(vm, proto, "_enter_set", tag_enter_leave_setter_protected) != 0
     || register_host_fn(vm, proto, "_leave_set", tag_enter_leave_setter_protected) != 0
     || register_native_method_tag(vm, proto, "new",      tag_new_native)      != URBI_OK
     || register_native_method_tag(vm, proto, "stop",     tag_stop_native)     != URBI_OK
     || register_native_method_tag(vm, proto, "freeze",   tag_freeze_native)   != URBI_OK
     || register_native_method_tag(vm, proto, "unfreeze", tag_unfreeze_native) != URBI_OK
     || register_native_method_tag(vm, proto, "block",    tag_block_native)    != URBI_OK
     || register_native_method_tag(vm, proto, "unblock",  tag_unblock_native)  != URBI_OK
     || register_native_method_tag(vm, proto, "frozen",   tag_frozen_native)   != URBI_OK
     || register_native_method_tag(vm, proto, "blocked",  tag_blocked_native)  != URBI_OK
     || register_native_method_tag(vm, proto, "enter",    tag_enter_native)    != URBI_OK
     || register_native_method_tag(vm, proto, "leave",    tag_leave_native)    != URBI_OK) {
        vm->tag_proto = NULL;
        vm->atom_tag  = NULL;
        return UVM_OOM;
    }
    return UVM_OK;
}
