/* SPDX-License-Identifier: BSD-3-Clause */
/* src/stdlib/temporal.c — v0.9.4 Phase 5: every() periodic-spawn primitive.
 *
 * Approach: per-call UPeriodic records on a singly linked list rooted at
 * vm->periodics_head.  urbi_step pumps the list on each call; expired
 * periodics spawn a body strand via the same do_spawn_body_coroutine-style
 * sequence the watcher subsystem uses.  No new opcode, no synthesized
 * UProto, no wire-format change.
 *
 * Lifecycle:
 *   - every_native(period_us, body) allocates a UPeriodic, sets
 *     next_fire_us = now + period_us, head-inserts on periodics_head.
 *   - urbi_step → urbi_periodic_pump fires due periodics by spawning a
 *     body strand and linking via s->periodic_owner.
 *   - On body-strand DEAD, exit_strand calls urbi_periodic_body_completed
 *     which clears current_strand and either re-arms (next_fire_us = now
 *     + period_us) or marks unregister_pending (UEXEC_CANCEL / TAG_STOP /
 *     uncaught THROW).
 *   - Next pump pass frees any periodic with unregister_pending set.
 *
 * Cancellation:
 *   The body strand inherits the caller's ambient tag chain.  When
 *   `mytag.stop()` unwinds the body to DEAD with UEXEC_TAG_STOP or
 *   UEXEC_CANCEL, the completion hook marks the periodic so no further
 *   re-spawn fires.
 *
 * GC reachability:
 *   urbi_periodic_table_walk_roots is registered with the GC root
 *   provider list at urbi_vm_init.  Each periodic's body closure is
 *   yielded as UVAL_CLOSURE.  Body strands themselves are reached via
 *   realm->strands_head (urbi_gc_sched_walk_roots).
 */

#include "stdlib/temporal.h"
#include "stdlib/object_root.h"       /* urbi_native_closure_create + raise helpers */

#include "chunk/uchunk.h"             /* UValue / UVAL_* */
#include "gc/ugc_incremental.h"       /* urbi_gc_shade_gray (owning_tag root, GC-03) */
#include "object/uchunk_instance.h"   /* UChunkInstance / UProtoInstanceArr */
#include "realm/urealm.h"             /* URealm */
#include "runtime/uclosure.h"         /* UClosure full definition */
#include "runtime/umacros.h"          /* URBI_INTERNAL_ASSERT, urbi_zero */
#include "runtime/ulist.h"            /* URBI_SLIST_PUSH, URBI_SLIST_UNLINK, URBI_SLIST_FOREACH_SAFE */
#include "sched/ustrand.h"            /* UEXEC_*, UStrand */
#include "sched/usched_cooperative.h" /* urbi_sched_strand_make_runnable (via urbi_strand_start) */
#include "urbi/types.h"               /* urbi_make_nil */
#include "urbi/urbi.h"                /* URBI_OK / URBI_ERR_* / urbi_realm_set_global / URBI_ASSERT_NOT_ISR / URBI_LOG_WARN */
#include "vm/uvm.h"                   /* UVM */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* === every_native ========================================================
 *
 * Signature on the urbi side: every(period_us, body) -> nil
 *
 * Args:
 *   args[0] = UVAL_INT — period in microseconds.  Time literals like
 *             `100ms` lex to UVAL_INT in microseconds (src/lex/ulex.c
 *             apply_duration_suffix).  UVAL_FLOAT is accepted as a
 *             courtesy and truncated.
 *   args[1] = UVAL_CLOSURE — body to invoke each period.  The parser
 *             desugar `every (E) S` ==> `every(E, function () { S })`
 *             always wraps the body in a function literal (see
 *             src/parse/uparse_react.c every-desugar).
 *
 * Returns UVAL_NIL.  Throws ArityError on wrong argc, TypeError on bad
 * argument types. */

static UPeriodic *
periodic_alloc(UVM *vm, UClosure *body, uint64_t period_us, URealm *realm)
{
    UPeriodic *p = (UPeriodic *)vm->alloc_fn(NULL, sizeof(UPeriodic),
                                             vm->alloc_ud);
    if (p == NULL) return NULL;
    urbi_zero(p, sizeof *p);
    p->body         = body;
    p->period_us    = period_us;
    p->realm        = realm;
    /* next_fire_us / owning_tag / current_strand / module_instance /
     * unregister_pending / next default to 0/NULL via urbi_zero. */
    return p;
}

/* Locate the UChunkInstance owning the body closure's proto_inst.  Mirrors
 * the cross-module_instance pointer-range walk in do_spawn_body_coroutine
 * (uwatcher_spawn.c).  Walked once at install time and cached on the
 * UPeriodic; later body-spawn iterations reuse the same module_instance.
 * Returns NULL if no owning instance is found (defensive — does not occur
 * in production paths). */
static UChunkInstance *
find_owning_module_instance(UVM *vm, const UClosure *body)
{
    if (body == NULL || body->proto_inst == NULL) return NULL;
    UChunkInstance *mi;
    for (mi = vm->module_instances_head; mi != NULL; mi = mi->next_in_vm) {
        UProtoInstanceArr *arr = mi->proto_instances;
        if (arr == NULL || arr->n == 0) continue;
        const UProtoInstance *first = &arr->entries[0];
        const UProtoInstance *last  = &arr->entries[arr->n - 1U];
        if (body->proto_inst >= first && body->proto_inst <= last) {
            return mi;
        }
    }
    return NULL;
}

/* Walk the caller strand's cleanup stack top-down looking for the
 * innermost UCLEANUP_TAG_SCOPE entry.  Returns NULL if no user tag has
 * been pushed (realm->tag is the fallback, which urbi_strand_create
 * attaches automatically). */
static struct UTag *
innermost_user_tag(const UStrand *caller, const URealm *realm)
{
    if (caller == NULL || caller->cleanup_base == NULL) return NULL;
    /* Walk top-down; first TAG_SCOPE whose owning_tag != realm->tag wins. */
    uint16_t i;
    for (i = caller->cleanup_depth; i > 0; i--) {
        const UCleanupEntry *e = &caller->cleanup_base[i - 1U];
        if (e->kind != UCLEANUP_TAG_SCOPE) continue;
        if (e->owning_tag != NULL && e->owning_tag != realm->tag) {
            return e->owning_tag;
        }
    }
    return NULL;
}

static int
every_native(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self;
    if (nargs != 2) {
        return urbi_raise_arity(vm, "every", 2, nargs, out);
    }

    /* Period: UVAL_INT in microseconds.  Lex emits UVAL_INT for
     * `100ms` (already scaled to us in src/lex/ulex.c).  Accept UVAL_FLOAT
     * as a courtesy and truncate. */
    uint64_t period_us;
    if (args[0].kind == (uint8_t)UVAL_INT) {
        int64_t v = args[0].v.i;
        if (v <= 0) {
            return urbi_raise_type(vm,
                "every: period must be positive", out);
        }
        period_us = (uint64_t)v;
    } else if (args[0].kind == (uint8_t)UVAL_FLOAT) {
        /* SCHED-14 (owner-decided 2026-06-11): bare float = SECONDS,
         * matching sleep().  Duration literals (100ms) reach here as
         * UVAL_INT microseconds via the lexer and are unaffected.
         *
         * !(f > 0.0) rejects NaN, negatives, and zero in one test; the upper
         * bound rejects +inf and values whose µs conversion would overflow
         * int64 — (uint64_t)(int64_t)(f * 1e6) is UB for out-of-range f.
         *
         * args[0].v.f is double on f64 builds, float on f32; promotion to
         * double for the comparison is automatic. */
        double f = (double)args[0].v.f;
        if (!(f > 0.0) || f > 9.2e12) {
            return urbi_raise_type(vm,
                "every: period must be positive", out);
        }
        /* Cast via int64_t to reuse __fixdfdi rather than __fixunsdfdi.
         * Guard above guarantees f > 0 and f * 1e6 <= 9.2e18 < INT64_MAX,
         * so the int64_t cast is defined and the subsequent uint64_t cast is
         * safe. */
        period_us = (uint64_t)(int64_t)(f * 1e6);
    } else {
        return urbi_raise_type(vm,
            "every: period must be a Duration (Integer microseconds)", out);
    }

    if (args[1].kind != (uint8_t)UVAL_CLOSURE || args[1].v.p == NULL) {
        return urbi_raise_type(vm,
            "every: body must be a Function", out);
    }
    UClosure *body = (UClosure *)args[1].v.p;

    /* Resolve realm + ambient tag from the calling strand.  vm->cur_strand
     * is the strand currently in OP_CALL — set by urbi_step before
     * urbi_vm_dispatch_loop_until_yield. */
    UStrand *caller = vm->cur_strand;
    URealm  *realm  = (caller != NULL) ? caller->realm : NULL;
    if (realm == NULL) {
        realm = urbi_realm_global(vm);
    }
    if (realm == NULL) {
        return urbi_raise_oom(vm, out);
    }

    UPeriodic *p = periodic_alloc(vm, body, period_us, realm);
    if (p == NULL) {
        return urbi_raise_oom(vm, out);
    }
    p->owning_tag      = innermost_user_tag(caller, realm);
    p->module_instance = find_owning_module_instance(vm, body);

    /* Schedule the first fire at now + period_us.  host_time_us is
     * required for the periodic to fire (urbi_step also requires it for
     * the sleep queue). */
    uint64_t now = 0U;
    if (vm->host_time_us != NULL) now = vm->host_time_us(vm->host_time_ud);
    p->next_fire_us = now + period_us;

    URBI_SLIST_PUSH(vm->periodics_head, p, next);

    *out = urbi_make_nil();
    return UEXEC_OK;
}

/* === sleep_native ========================================================
 *
 * W6/v0.10.2: sleep(duration) stdlib C-native — blocks current strand via
 * USTRAND_REASON_SLEEP.  Closes legacy audit F15 + v0.9.4-era Pico
 * follow-up (whenever(named_event) workaround required C-side watcher;
 * native sleep unblocks the simplest blocking-wait pattern from script).
 *
 * Duration accepted as:
 *   UVAL_INT   — microseconds.  Matches the time-literal lexer output:
 *                `100ms` → UVAL_INT(100000), `1s` → UVAL_INT(1000000).
 *   UVAL_FLOAT — seconds, converted via *1e6 (same scale as the int form).
 * Other kinds raise TypeError.  Negative values raise TypeError.
 *
 * Blocks the current strand by calling urbi_sched_strand_block(s,
 * USTRAND_REASON_SLEEP, now_us + duration_us).  The scheduler's existing
 * sleep-queue infrastructure wakes the strand when host_time_us() reaches
 * the target.
 *
 * TAG_STOP on a sleeping strand wakes it via the existing
 * urbi_sched_strand_unblock path in urbi_tag_stop's member_strands walk
 * (src/runtime/uunwind.c) — verified at v0.10.2 W6.
 *
 * Returns nil after wakeup (or when TAG_STOP interrupts the sleep;
 * the TAG_STOP unwind delivers UEXEC_TAG_STOP before the nil return
 * can be observed by the caller). */
static int
sleep_native(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)self;
    if (nargs != 1) {
        return urbi_raise_arity(vm, "sleep", 1, nargs, out);
    }
    uint64_t duration_us;
    if (args[0].kind == (uint8_t)UVAL_INT) {
        int64_t i = args[0].v.i;
        if (i < 0) {
            return urbi_raise_type(vm, "sleep: negative duration", out);
        }
        duration_us = (uint64_t)i;
    } else if (args[0].kind == (uint8_t)UVAL_FLOAT) {
        double f = (double)args[0].v.f;
        /* SCHED-14: align guard with every() — reject NaN, negatives, +inf,
         * and too-large values (UB on µs conversion) in one expression.
         * !(f >= 0.0) catches NaN and negatives; f > 9.2e12 catches +inf
         * and overflow-on-µs-conversion values. */
        if (!(f >= 0.0) || f > 9.2e12) {
            return urbi_raise_type(vm, "sleep: negative duration", out);
        }
        /* Cast via int64_t to reuse __fixdfdi (double→int64, already in the
         * freestanding-golden symbol list) rather than pulling in the
         * distinct __fixunsdfdi (double→uint64) libgcc helper.  The f >= 0
         * and f <= 9.2e12 checks above guarantee the int64_t cast is safe. */
        duration_us = (uint64_t)(int64_t)(f * 1e6);
    } else {
        return urbi_raise_type(vm,
            "sleep: duration must be UVAL_INT (microseconds) or UVAL_FLOAT (seconds)",
            out);
    }

    UStrand *cur = vm->cur_strand;
    URBI_INTERNAL_ASSERT(cur != NULL);
    /* refactor-4 B10/SCH4-01: run-to-completion (transient) entry cannot park.
     * A zero sleep is a no-op — return nil immediately.  Real strands keep
     * their yield-shaped zero sleep (it lets watchers fire between statements). */
    if (cur->is_transient_strand && duration_us == 0U) {
        *out = urbi_make_nil();
        return UEXEC_OK;
    }
    uint64_t now_us = (vm->host_time_us != NULL) ? vm->host_time_us(vm->host_time_ud) : 0U;
    urbi_sched_strand_block(cur, USTRAND_REASON_SLEEP, now_us + duration_us);
    /* urbi_sched_strand_block puts the strand on the sleep queue; the scheduler
     * resumes it when wake_us elapses (or earlier via TAG_STOP).
     * Returns nil after wakeup — the strand yields immediately here and
     * the scheduler drives the actual wait. */
    *out = urbi_make_nil();
    return UEXEC_OK;
}

/* === urbi_temporal_native_register =====================================
 *
 * Allocates vm->every_native_closure and stores it.  Realm-global binding
 * is deferred to urbi_temporal_native_register_globals so the registry's
 * slot 0..7 layout (Object .. List) is untouched. */

int
urbi_temporal_native_register(UVM *vm)
{
    if (vm == NULL) return URBI_ERR_INVALID_ARG;
    if (vm->every_native_closure != NULL) return URBI_OK;  /* idempotent */
    UClosure *cl = urbi_native_closure_create(vm, every_native);
    if (cl == NULL) return URBI_ERR_OOM;
    vm->every_native_closure = cl;
    return URBI_OK;
}

int
urbi_temporal_native_register_globals(UVM *vm, URealm *realm)
{
    if (vm == NULL || realm == NULL) return URBI_ERR_INVALID_ARG;

    /* Bind "every" as a realm global pointing at vm->every_native_closure. */
    if (vm->every_native_closure != NULL) {
        UValue ev;
        ev.kind = (uint8_t)UVAL_CLOSURE;
        ev.v.p  = (void *)vm->every_native_closure;
        int rc = urbi_realm_set_global(vm, realm, "every", 5, ev);
        if (rc != URBI_OK) return rc;
    }

    /* W6/v0.10.2: bind "sleep" as a realm global.  Allocate the closure
     * here (no UVM field — GC reachability via the realm-global slot,
     * same as the comment above for every_native_closure).  One
     * allocation per realm creation; the realm's global_object slot keeps
     * the closure alive for the lifetime of the realm. */
    {
        UClosure *sl_cl = urbi_native_closure_create(vm, sleep_native);
        if (sl_cl == NULL) return URBI_ERR_OOM;
        UValue sv;
        sv.kind = (uint8_t)UVAL_CLOSURE;
        sv.v.p  = (void *)sl_cl;
        int rc = urbi_realm_set_global(vm, realm, "sleep", 5, sv);
        if (rc != URBI_OK) return rc;
    }

    return URBI_OK;
}

/* === GC root walker ==================================================== */

void
urbi_periodic_table_walk_roots(UVM *vm, UGcRootCallback cb, void *ctx)
{
    /* No URBI_ASSERT_NOT_ISR — root walkers run from the GC slice path
     * (mirrors urbi_gc_watcher_table_walk_roots). */
    UPeriodic *p;
    for (p = vm->periodics_head; p != NULL; p = p->next) {
        if (p->body != NULL) {
            UValue tmp;
            tmp.kind = (uint8_t)UVAL_CLOSURE;
            tmp.v.p  = (void *)p->body;
            cb(vm, &tmp, ctx);
        }
        /* current_strand reachable via realm->strands_head (sched walker). */
        if (p->owning_tag != NULL) {   /* GC-03: UTag is GC-managed since M5 */
            urbi_gc_shade_gray(vm, (UCell *)p->owning_tag);
        }
    }
    /* vm->every_native_closure is reached separately via vm_misc_walk_roots
     * extension below.  Allocated as a GC cell, but the only live pointer
     * is the UVM-field — needs an explicit yield to survive collection.
     *
     * Actually: it is referenced by the realm-global slot for "every"
     * once urbi_temporal_native_register_globals runs, which the
     * urbi_gc_realm_list_walk_roots provider already reaches via
     * realm->global_object's slot table.  So no extra yield needed here
     * for that closure — covered by urbi_gc_realm_list_walk_roots.  Add the
     * explicit yield only if a path other than the global binding makes
     * the closure escape the realm.  None today. */
    if (vm->every_native_closure != NULL) {
        UValue tmp;
        tmp.kind = (uint8_t)UVAL_CLOSURE;
        tmp.v.p  = (void *)vm->every_native_closure;
        cb(vm, &tmp, ctx);
    }
}

/* === Periodic body spawn ===============================================
 *
 * Mirrors do_spawn_body_coroutine in uwatcher_spawn.c.  Returns the body
 * strand on success; on failure logs a warning and returns NULL so the
 * pump can leave the periodic re-armed for the next pass. */
static UStrand *
spawn_periodic_body(UVM *vm, UPeriodic *p)
{
    URBI_ASSERT_NOT_ISR(vm);

    /* Step 1: allocate body strand (DORMANT). */
    UStrand *body = urbi_strand_create(vm, p->realm, p->body);
    if (body == NULL) {
        if (vm->host_log_fn != NULL) {
            vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
                "every: body spawn failed (strand alloc OOM)");
        }
        return NULL;
    }

    /* Step 2: inherit owning_tag only when distinct from realm->tag.
     * urbi_strand_create already attaches realm->tag at depth 0. */
    if (p->owning_tag != NULL && p->owning_tag != p->realm->tag) {
        struct UTag *chain[1];
        chain[0] = p->owning_tag;
        urbi_strand_attach_ambient_tags(body, chain, 1);
        if (body->state == USTRAND_STATE_DEAD) {
            urbi_strand_destroy(vm, body);
            if (vm->host_log_fn != NULL) {
                vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
                    "every: body spawn ambient-attach overflow");
            }
            return NULL;
        }
    }

    /* Step 3: arm — allocates register stack, wires pc / R / frame_count. */
    if (urbi_strand_arm_from_closure(body, p->body, /*nargs=*/0) != 0) {
        urbi_strand_destroy(vm, body);
        if (vm->host_log_fn != NULL) {
            vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
                "every: body spawn failed (stack alloc OOM)");
        }
        return NULL;
    }

    /* Step 3b: bind root_proto so OP_CLOSURE at frame_count==0 finds
     * body_proto->nested[] (reactive F4 fix).  arm_from_closure wires
     * pc/pc_base/cur_consts from body->proto but does NOT set root_proto;
     * without this OP_CLOSURE's executing_proto = s->root_proto is NULL
     * and "CLOSURE: proto index out of range" halts the body strand. */
    body->root_proto = p->body->proto;
    urbi_proto_strand_ref_acquire(body->root_proto, URBI_PROTO_REF_OWNER_STRAND);

    /* Step 4: wire module_instance for IC resolution at frame_count == 0. */
    body->module_instance = p->module_instance;

    /* Step 5: wire back-pointers. */
    body->periodic_owner = p;
    p->current_strand    = body;

    /* Step 6: DORMANT -> READY (enqueue on run-queue). */
    urbi_strand_start(vm, body);

    return body;
}

/* Internal helper: detach periodic p from vm->periodics_head and free.
 * NOT idempotent — caller must guarantee p is on the list.  Used only by
 * urbi_periodic_pump's teardown sweep, so the listed-on-vm invariant
 * holds by construction. */
static void
periodic_unlink_and_free(UVM *vm, UPeriodic *p)
{
    /* Caller contract: p is on vm->periodics_head (called only from the Phase 2
     * teardown sweep where unregister_pending is confirmed). */
    URBI_SLIST_UNLINK(vm->periodics_head, p, next, UPeriodic);
    p->next = NULL;
    vm->alloc_fn(p, 0, vm->alloc_ud);
}

/* === urbi_periodic_pump ================================================
 *
 * Called from urbi_step after the sleep-queue drain.  Two passes:
 *   1. Fire any periodic with current_strand == NULL && next_fire_us <= now.
 *   2. Free any periodic with unregister_pending set AND current_strand == NULL.
 *
 * Returns 1 when any periodic record is alive (live or pending teardown
 * with an in-flight strand), 0 when the list is empty. */
int
urbi_periodic_pump(UVM *vm)
{
    if (vm == NULL || vm->periodics_head == NULL) return 0;
    URBI_ASSERT_NOT_ISR(vm);

    uint64_t now = 0U;
    if (vm->host_time_us != NULL) now = vm->host_time_us(vm->host_time_ud);

    /* Phase 1: fire due periodics.  Walk the list; spawn into any slot
     * with current_strand == NULL && fire time reached && not pending
     * unregister.  spawn_periodic_body sets p->current_strand on success. */
    UPeriodic *p;
    for (p = vm->periodics_head; p != NULL; p = p->next) {
        if (p->unregister_pending) continue;
        if (p->current_strand != NULL) continue;
        if (p->next_fire_us > now) continue;
        (void)spawn_periodic_body(vm, p);
        /* On spawn failure, leave p in place; next pump pass retries. */
    }

    /* Phase 2: teardown sweep.  Free periodics with unregister_pending
     * set AND no in-flight body strand.  An unregister with current_strand
     * still alive defers the free until the strand reaches DEAD and
     * urbi_periodic_body_completed clears the back-pointer. */
    {
        UPeriodic *p2, *next;
        URBI_SLIST_FOREACH_SAFE(p2, next, vm->periodics_head, next) {
            if (p2->unregister_pending && p2->current_strand == NULL) {
                periodic_unlink_and_free(vm, p2);
            }
        }
    }

    return vm->periodics_head != NULL ? 1 : 0;
}

/* === urbi_periodic_body_completed ======================================
 *
 * Called from uvm.c::exit_strand for any strand whose periodic_owner is
 * non-NULL.  Snapshot the strand's fatal_status (cleared on destroy) and
 * decide whether to re-arm the periodic or mark it for unregister.
 *
 * Semantics (spec §11.4):
 *   - UEXEC_OK              -> re-arm (body+sleep cadence).
 *   - UEXEC_THROW           -> unregister (uncaught exception kills it).
 *   - UEXEC_CANCEL          -> unregister (mytag.stop() cascade or realm destroy).
 *   - UEXEC_TAG_STOP        -> unregister.
 *
 * After this hook returns, ustep.c's eager DEAD-reap path frees the
 * strand.  The next urbi_periodic_pump pass then either re-fires (if
 * re-armed) or unlinks-and-frees the periodic (if unregistered). */
void
urbi_periodic_body_completed(UVM *vm, UStrand *s)
{
    if (s == NULL) return;
    UPeriodic *p = s->periodic_owner;
    if (p == NULL) return;

    URBI_INTERNAL_ASSERT(p->current_strand == s);

    /* Clear back-pointers BEFORE deciding re-arm vs unregister so that any
     * re-entrant pump pass observes a consistent (current_strand == NULL)
     * state for this periodic. */
    s->periodic_owner   = NULL;
    p->current_strand   = NULL;

    if (s->fatal_status == UEXEC_OK || s->fatal_status == UEXEC_RETURN) {
        /* SCHED-14 (owner-decided 2026-06-11): FIXED cadence (legacy every|
         * semantics).  The deadline advances by one period from the PREVIOUS
         * deadline so the firing interval does not drift with body duration.
         *
         * On overrun (body duration >= period, next_fire_us already in the
         * past), the deadline resumes at now + period_us: the missed periods
         * are SKIPPED with no burst of catch-up iterations and NO immediate
         * catch-up fire.
         *
         * CONTROLLER-RATIFIED DEVIATION (pending owner confirmation at tag
         * close-out) from the literal SCHED-14 "slide-to-now / single late
         * fire": resuming at `now` (rather than `now + period_us`) makes the
         * periodic perpetually-due whenever body-duration >= period.  The
         * deadline is read from the completion clock here, but the periodic
         * pump re-reads the (later) clock; on an advancing clock that deadline
         * is then always <= the pump's read, so the body re-fires every pump
         * pass within the same urbi_step and the scheduler never returns
         * anything but RUNNING -- a 100% CPU non-quiescence hang (empirically:
         * basic.chk 30s timeout, nested.chk 180s timeout).  Resuming at
         * now + period_us lets the VM reach WAKE_AT/QUIESCENT between overrun
         * fires -- which a cooperative embedded runtime requires -- and avoids
         * double-actuation on robotics hardware. */
        uint64_t now = 0U;
        if (vm->host_time_us != NULL) now = vm->host_time_us(vm->host_time_ud);
        p->next_fire_us += p->period_us;
        if (p->next_fire_us <= now) p->next_fire_us = now + p->period_us;
    } else {
        /* UEXEC_THROW / UEXEC_CANCEL / UEXEC_TAG_STOP: stop firing. */
        p->unregister_pending = 1U;
    }
}

/* === urbi_periodic_destroy_for_realm ===================================
 *
 * Called from urbi_realm_destroy BEFORE the realm's strands_head sweep
 * frees in-flight body strands.  For every periodic owned by realm `r`:
 *   - mark unregister_pending so no further re-spawn fires;
 *   - clear current_strand so the back-pointer doesn't dangle when the
 *     realm-destroy strand sweep frees the body strand directly via
 *     urbi_strand_destroy (which bypasses exit_strand and therefore
 *     would not have called urbi_periodic_body_completed).
 * Also walk the realm's strands and clear any periodic_owner back-pointer
 * (defensive against double-free if the same strand were touched twice). */
void
urbi_periodic_destroy_for_realm(UVM *vm, const URealm *r)
{
    if (vm == NULL || r == NULL) return;
    UPeriodic *p;
    for (p = vm->periodics_head; p != NULL; p = p->next) {
        if (p->realm == r) {
            if (p->current_strand != NULL) {
                p->current_strand->periodic_owner = NULL;
                p->current_strand = NULL;
            }
            p->unregister_pending = 1U;
        }
    }
}

/* === urbi_periodic_destroy_all =========================================
 *
 * Called from urbi_vm_destroy AFTER all realms are torn down — strands_head
 * sweeps already freed every in-flight body strand and cleared
 * current_strand back-pointers.  Walk and free the entire list. */
void
urbi_periodic_destroy_all(UVM *vm)
{
    if (vm == NULL) return;
    UPeriodic *p = vm->periodics_head;
    while (p != NULL) {
        UPeriodic *next = p->next;
        vm->alloc_fn(p, 0, vm->alloc_ud);
        p = next;
    }
    vm->periodics_head = NULL;
}

/* === urbi_periodic_earliest_wake_us ==================================== */

uint64_t
urbi_periodic_earliest_wake_us(const UVM *vm)
{
    if (vm == NULL) return UINT64_MAX;
    uint64_t earliest = UINT64_MAX;
    const UPeriodic *p;
    for (p = vm->periodics_head; p != NULL; p = p->next) {
        if (p->unregister_pending) continue;
        if (p->current_strand != NULL) continue;
        if (p->next_fire_us < earliest) {
            earliest = p->next_fire_us;
        }
    }
    return earliest;
}

/* === urbi_periodics_stop_owned_by / urbi_tag_owns_periodic =============
 *
 * B5 / SCHED-N2 (refactor-4, 2026-07-04): tag.stop() must cascade to the
 * periodic list so the flagship `t: every(P) body(); t.stop()` idiom works.
 *
 * urbi_periodics_stop_owned_by: walk vm->periodics_head; for every periodic
 * whose owning_tag matches, set unregister_pending.  The next
 * urbi_periodic_pump pass (Phase 2) then frees any such periodic whose
 * current_strand is NULL.  Called from urbi_tag_stop (uunwind.c) after the
 * member-watcher cascade.
 *
 * urbi_tag_owns_periodic: returns true if at least one non-unregistered
 * periodic has owning_tag == tag.  Called from tag_stop_native (utag_native.c)
 * so a tag that owns a live periodic is never treated as "no active scope"
 * by the D3 fatal-escalation check. */

void
urbi_periodics_stop_owned_by(UVM *vm, const struct UTag *tag)
{
    UPeriodic *p = vm->periodics_head;
    while (p != NULL) {
        if (p->owning_tag == tag) p->unregister_pending = 1U;
        p = p->next;
    }
}

bool
urbi_tag_owns_periodic(const struct UVM *vm, const struct UTag *tag)
{
    const UPeriodic *p = vm->periodics_head;
    while (p != NULL) {
        if (p->owning_tag == tag && !p->unregister_pending) return true;
        p = p->next;
    }
    return false;
}
