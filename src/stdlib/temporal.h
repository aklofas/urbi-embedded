/* SPDX-License-Identifier: BSD-3-Clause */
/* src/stdlib/temporal.h — v0.9.4 Phase 5: every() periodic-spawn primitive.
 *
 * `every (period) body` desugars at parse time to a regular call
 * `every(period_us, function() { body })`.  This TU implements the runtime
 * backing — a stdlib C-native function `every` plus the per-VM registry
 * that drives periodic body re-spawn from urbi_step.
 *
 * Approach: a small UPeriodic record per call, threaded onto a singly
 * linked list on UVM.  Each periodic carries the body closure, the period
 * in microseconds, the next fire time, and (when a body strand is alive)
 * a back-pointer to the current body strand.  urbi_step walks the list
 * on each call: any periodic whose next_fire_us has elapsed and whose
 * current_strand is NULL spawns a fresh body strand and re-arms the
 * fire time.
 *
 * Cancellation: the body strand inherits the caller's ambient tag chain
 * (mirrors do_spawn_body_coroutine for watcher bodies).  When mytag.stop()
 * unwinds the body strand to DEAD with UEXEC_TAG_STOP or UEXEC_CANCEL, the
 * periodic-completion hook marks the periodic for unregister so no further
 * re-spawn fires; the next ustep sweep walks the list and frees it.
 *
 * GC reachability: a root walker registered with the GC at urbi_vm_init
 * yields each periodic's body closure to the mark callback.  Current body
 * strands are reachable via realm->strands_head (sched_walk_roots).
 *
 * No new opcode.  No wire-format change.  ABI is additive only — new
 * field on UVM (periodics_head) + new field on UStrand (periodic_owner).
 */

#ifndef URBI_STDLIB_TEMPORAL_H
#define URBI_STDLIB_TEMPORAL_H

#include <stdint.h>

#include "urbi/types.h"   /* UValue (needed for UGcRootCallback signature) */
#include "gc/ugc.h"       /* UGcRootCallback */

#ifdef __cplusplus
extern "C" {
#endif

struct UVM;
struct URealm;
struct UTag;
struct UStrand;
struct UClosure;
struct UChunkInstance;

/* UPeriodic — per-call record for `every (period) body`.
 *
 * Allocated via vm->alloc_fn at every_native() time; freed when the periodic
 * is unregistered (tag cancel, realm destroy, or VM destroy).
 *
 * body                — captured body closure (GC-managed; reachable via
 *                       periodic_table_walk_roots).
 * period_us           — duration between consecutive fires.  Microseconds
 *                       so time literals like `100ms` (already in us at
 *                       lex time) pass through unscaled.
 * next_fire_us        — host_time_us() value at which the next body
 *                       spawn should happen.
 * realm               — realm under which body strands are created.
 * owning_tag          — innermost ambient tag at install time, or NULL
 *                       when the install ran without a user tag scope
 *                       (defaults to realm->tag).  Matches the watcher
 *                       owning_tag field shape (uwatcher.h).
 * current_strand      — non-NULL while a body strand is in flight;
 *                       cleared by the completion hook when the body dies.
 *                       The body+sleep cadence (spec §11.4) means at most
 *                       one body runs at a time per periodic.
 * module_instance     — UChunkInstance to bind on body strands so
 *                       OP_GETSLOT / OP_SETSLOT inside the body can
 *                       resolve the IC table at frame_count==0.  Walked
 *                       once at install time (same scheme as
 *                       do_spawn_body_coroutine, uwatcher_spawn.c).
 * unregister_pending  — set when the periodic should not re-fire (body
 *                       cancelled via tag.stop, body threw uncaught, or
 *                       host called for teardown).  The next ustep sweep
 *                       walks the list and frees any with this flag set.
 * next                — singly-linked list link on vm->periodics_head.
 */
typedef struct UPeriodic {
    struct UClosure        *body;
    uint64_t                period_us;
    uint64_t                next_fire_us;
    struct URealm          *realm;
    struct UTag            *owning_tag;
    struct UStrand         *current_strand;
    struct UChunkInstance  *module_instance;
    struct UPeriodic       *next;
    uint8_t                 unregister_pending;
    uint8_t                 pad[7];
} UPeriodic;

/* urbi_temporal_native_register
 *
 * Allocates a native closure wrapping every_native and stores it on
 * vm->every_native_closure.  Realm-global binding for "every" is deferred
 * to urbi_temporal_native_register_globals (post-loop hook).
 *
 * Idempotent: re-entry returns URBI_OK without re-allocating.
 * Returns URBI_OK / URBI_ERR_OOM / URBI_ERR_INVALID_ARG. */
int urbi_temporal_native_register(struct UVM *vm);

/* urbi_temporal_native_register_globals
 *
 * Post-registry hook: bind "every" as a realm-global slot on realm->global_object,
 * pointing at vm->every_native_closure.  Lands at slot 15+ past the v1.0
 * packed-flag CONSTANT enforcement range. */
int urbi_temporal_native_register_globals(struct UVM *vm, struct URealm *realm);

/* GC root walker (registered with urbi_gc_register_root_provider at
 * urbi_vm_init).  Yields each periodic's body closure to the GC mark
 * callback and shades owning_tag (GC-managed UTag, refactor-3 GC-03).
 * Signature matches UGcRootProviderFn in gc/ugc.h. */
void urbi_periodic_table_walk_roots(struct UVM *vm,
                                    UGcRootCallback cb,
                                    void *ctx);

/* urbi_periodic_pump
 *
 * Walk vm->periodics_head: free any with unregister_pending set; for
 * periodics with current_strand == NULL and next_fire_us <= now, spawn
 * a fresh body strand.  Called from urbi_step after the sleep-queue drain.
 *
 * Returns 1 if any periodic record is alive (live or pending teardown),
 * 0 otherwise.  Callers use the return to decide whether the VM is fully
 * quiescent (analogous to wakeup_pending_count). */
int urbi_periodic_pump(struct UVM *vm);

/* urbi_periodic_body_completed
 *
 * Called from uvm.c::exit_strand for any strand whose periodic_owner is
 * non-NULL.  Clears the back-pointer, decides whether to re-arm the
 * periodic (clean OP_RET / fall-off-end) or mark it for unregister
 * (UEXEC_THROW / UEXEC_CANCEL / UEXEC_TAG_STOP).
 *
 * The function mirrors urbi_watcher_body_completed (uwatcher_spawn.c). */
void urbi_periodic_body_completed(struct UVM *vm, struct UStrand *s);

/* urbi_periodic_destroy_for_realm
 *
 * Walk vm->periodics_head and mark every record whose realm == r for
 * unregister_pending=1.  Called from urbi_realm_destroy so periodics
 * installed under a destroyed realm stop firing.  Does not free records
 * here (in-flight body strands may still be on the realm's strands_head
 * and the ustep sweep handles teardown once they reach DEAD). */
void urbi_periodic_destroy_for_realm(struct UVM *vm, const struct URealm *r);

/* urbi_periodic_destroy_all
 *
 * Free every periodic on vm->periodics_head.  Called from urbi_vm_destroy
 * after all realms have been torn down. */
void urbi_periodic_destroy_all(struct UVM *vm);

/* urbi_periodic_earliest_wake_us
 *
 * Returns the smallest next_fire_us across all live periodics, or
 * UINT64_MAX when no live periodic exists.  Read-only (const vm — callable
 * from urbi_vm_liveness).  Used by urbi_step to compute URBI_STEP_WAKE_AT. */
uint64_t urbi_periodic_earliest_wake_us(const struct UVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* URBI_STDLIB_TEMPORAL_H */
