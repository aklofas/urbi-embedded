/* SPDX-License-Identifier: BSD-3-Clause */
/* Row 11 / row 12 §3 — , and & separator runtime.
 * Implements OP_FORK_DETACH, OP_FORK_JOIN, and OP_JOIN_WAIT dispatch bodies,
 * plus the fork_wake_joiners() helper called at strand-DEAD transitions.
 *
 * M3 CLOSURE-SPAWN semantics vs. spec §7.1 shared-frame semantics:
 *   Pre-M2 separator-semantics spec §7.1 specifies that ,-spawned strands SHARE
 *   the parent's register frame (lexical sharing, per legacy comma-environment.chk).
 *   This M3 implementation uses CLOSURE-SPAWN instead because:
 *     (a) urbi_strand_create() accepts an entry_closure — shared-frame spawn has
 *         no API hook at M3.
 *     (b) Cross-strand stack-frame sharing + concurrent upvalue mutation requires
 *         substantial infrastructure not present at M3.
 *     (c) Design-risks item 7 (comma-environment.chk) and item 11 (,-detached +
 *         tag-scope cancel) are explicitly deferred to M5+.
 *     (d) No production urbiscript code at M3 depends on shared-frame semantics.
 *   T44 legacy chk corpus port: comma-environment.chk should be marked
 *   "# defer-to: M5" when ported, per design-risks item 7.
 *   TODO(M5+/design-risks-7): implement shared-frame spawn to satisfy spec §7.1.
 *
 * Freestanding-strict: vm->alloc_fn for all alloc/free.
 * No <stdlib.h>, <string.h>, or <assert.h> — use URBI_INTERNAL_ASSERT.
 */

#include "vm/uop_fork.h"
#include "module/umodule.h"     /* uinstr_a, uinstr_b, UOpcode, UClosure (fwd), UProto */
#include "runtime/uclosure.h"    /* UClosure full definition (M4: embeds UCell) */
#include "sched/ustrand.h"     /* UStrand, urbi_strand_create, urbi_strand_start, ... */
#include "sched/usched_cooperative.h"  /* sched_strand_block, sched_strand_make_runnable */
#include "runtime/ucleanup.h"    /* URBI_CLEANUP_MAX */
#include "urbi/urbi.h"        /* URBI_ASSERT_NOT_ISR */
#include "runtime/umacros.h"  /* URBI_INTERNAL_ASSERT */
#include "vm/uvm.h"         /* UVM, UVM_STACK_CAP, vm->alloc_fn */
#include "vm/uvm_internal.h" /* vm_format_type_error_msg (VM-003) */
#include "runtime/uframe.h"      /* UVM_STACK_CAP (also in uvm.h → uframe.h) */
#include <stddef.h>
#include <stdint.h>

/* ===================================================================
 * Internal helpers
 * =================================================================== */

/* Spawn a child strand running `child_closure` under `s->realm`.
 * Inherits s's full ambient tag chain (all TAG_SCOPE cleanup entries).
 * On success, child is DORMANT (ready to be started by caller).
 * On OOM or missing realm: sets s->fatal_status = UEXEC_CANCEL,
 * s->fatal_value = NIL, s->state = USTRAND_STATE_DEAD; returns NULL. */
static UStrand *
fork_spawn_child(UStrand *s, UClosure *child_closure)
{
    struct UTag *chain[URBI_CLEANUP_MAX];
    size_t n;

    /* Precondition: caller (dispatch body in uvm.c) already checked realm != NULL. */
    URBI_INTERNAL_ASSERT(s->realm != NULL);

    UStrand *child = urbi_strand_create(s->realm, child_closure);
    if (child == NULL) {
        s->fatal_status      = UEXEC_CANCEL;
        s->fatal_value.kind  = (uint8_t)UVAL_NIL;
        s->fatal_value.v.i   = 0;
        s->state             = USTRAND_STATE_DEAD;
        return NULL;
    }

    /* Inherit parent's ambient tag chain.
     * child->cleanup_depth is already 1 (realm->tag attached by urbi_strand_create).
     * We capture ALL of the parent's chain (including realm->tag) so that the
     * child sees the same full tag scope as the parent at the moment of fork.
     * Note: urbi_strand_create already attached realm->tag to child; passing the
     * entire chain again would double-attach realm->tag.  So we capture the
     * EXTRA tags above the realm->tag (indices 1..n-1 of the parent's chain).
     *
     * Actually: urbi_strand_attach_ambient_tags pushes additional entries on top
     * of whatever the child already has.  urbi_strand_create already attached
     * realm->tag as entry[0].  We therefore capture the parent chain and skip
     * the first element (realm->tag) which the child already has, then attach
     * entries 1..n-1.
     *
     * Edge case: if parent has NO ambient tags beyond realm->tag (n <= 1), no
     * additional attach is needed. */
    n = urbi_strand_capture_ambient_chain(s, chain, URBI_CLEANUP_MAX);
    if (n > 1) {
        /* chain[0] is realm->tag (already on child); pass chain[1..n-1]. */
        urbi_strand_attach_ambient_tags(child, chain + 1, n - 1);
        if (child->state == USTRAND_STATE_DEAD) {
            /* OOM inside attach — child is already dead; treat as spawn-OOM. */
            urbi_strand_destroy(child);
            s->fatal_status      = UEXEC_CANCEL;
            s->fatal_value.kind  = (uint8_t)UVAL_NIL;
            s->fatal_value.v.i   = 0;
            s->state             = USTRAND_STATE_DEAD;
            return NULL;
        }
    }

    /* Arm child execution state from child_closure->proto.
     *
     * urbi_strand_create() leaves all execution fields zero-init
     * ("frame-0 setup deferred to urbi_step or a future urbi_strand_arm
     * helper" per ustrand.c).  urbi_strand_arm_from_closure is that helper.
     * The child runs with frame_count == 0, just like urbi_vm_run's transient
     * strand; OP_RET at frame_count == 0 writes to out_slot (NULL for
     * detached children) and transitions to DEAD cleanly.
     *
     * GETUPVAL/SETUPVAL at frame_count == 0 fall back to
     * s->entry_closure (already set by urbi_strand_create) — see uvm.c
     * OP_GETUPVAL dispatch. */
    if (urbi_strand_arm_from_closure(child, child_closure) != 0) {
        /* OOM allocating register stack — tear down child. */
        urbi_strand_destroy(child);
        s->fatal_status     = UEXEC_CANCEL;
        s->fatal_value.kind = (uint8_t)UVAL_NIL;
        s->fatal_value.v.i  = 0;
        s->state            = USTRAND_STATE_DEAD;
        return NULL;
    }

    child->module     = s->module;      /* diagnostics + nested-proto lookup */
    child->root_proto = s->root_proto;  /* v0.8.1 Phase 1: inherit fast-path alias */
    /* v0.8.1 Phase 2 (Variant B fusion): child strand-bind bump goes to
     * root_proto, not module->refcount.  child->root_proto is already set
     * above; ustrand_destroy will dec it at child death. */
    umodule_proto_refcount_inc(child->root_proto);
    /* CHSTR-014 (T102): inherit the parent's UModuleInstance pointer so that
     * OP_GETSLOT / OP_SETSLOT in the child can resolve the IC table at
     * frame_count == 0 (which reads s->module_instance->proto_instances
     * ->entries[0].ic_table).  ,-spawned and &-spawned siblings execute in
     * the same module as their parent, so the IC RAM tier is shared.
     * Without this assignment, the child runs with module_instance == NULL
     * and the first OP_GETSLOT in the child closure would dereference NULL.
     * The watcher body-spawn path (uwatcher_spawn.c) handles this differently
     * because the body closure's owning module may differ from the installing
     * strand's module; fork siblings have no such ambiguity. */
    child->module_instance = s->module_instance;
    /* child_closure is GC-managed; no manual ownership transfer needed. */

    return child;
}

/* ===================================================================
 * Public: OP_FORK_DETACH
 * =================================================================== */

int
op_fork_detach(UStrand *s, UVM *vm, uint32_t instr)
{
    uint8_t a = uinstr_a(instr);
    UClosure *child_closure;
    UStrand *child;

    URBI_ASSERT_NOT_ISR(vm);

    /* R[A] must hold a UVAL_CLOSURE (emitter guarantees this — VM-003).
     * Hand-crafted bytecode could leave a non-closure value here; without
     * this kind check release builds happily cast NIL.v.p (NULL) and
     * fork_spawn_child dereferences child_closure->proto. */
    if (s->R[a].kind != (uint8_t)UVAL_CLOSURE) {
        vm->last_error = UVM_TYPE_ERROR;
        vm_format_type_error_msg(vm,
            "OP_FORK_DETACH: register operand is not a closure");
        s->fatal_status     = UEXEC_CANCEL;
        s->fatal_value.kind = (uint8_t)UVAL_NIL;
        s->fatal_value.v.i  = 0;
        s->state            = USTRAND_STATE_DEAD;
        return -1;
    }
    child_closure = (UClosure *)s->R[a].v.p;

    child = fork_spawn_child(s, child_closure);
    if (child == NULL) {
        /* fork_spawn_child already set s->fatal_status and s->state = DEAD. */
        return -1;
    }

    /* DORMANT → READY: enqueue child at the tail of the run-queue. */
    urbi_strand_start(child);

    /* Parent continues with the next instruction. */
    return 0;
}

/* ===================================================================
 * Public: OP_FORK_JOIN
 * =================================================================== */

int
op_fork_join(UStrand *s, UVM *vm, uint32_t instr)
{
    uint8_t a = uinstr_a(instr);   /* closure register */
    uint8_t b = uinstr_b(instr);   /* child-handle output register */
    UClosure *child_closure;
    UStrand *child;

    URBI_ASSERT_NOT_ISR(vm);

    if (s->R[a].kind != (uint8_t)UVAL_CLOSURE) {
        vm->last_error = UVM_TYPE_ERROR;
        vm_format_type_error_msg(vm,
            "OP_FORK_JOIN: register operand is not a closure");
        s->fatal_status     = UEXEC_CANCEL;
        s->fatal_value.kind = (uint8_t)UVAL_NIL;
        s->fatal_value.v.i  = 0;
        s->state            = USTRAND_STATE_DEAD;
        return -1;
    }
    child_closure = (UClosure *)s->R[a].v.p;

    child = fork_spawn_child(s, child_closure);
    if (child == NULL) {
        return -1;
    }

    /* Store the child handle in R[B] so OP_JOIN_WAIT can find it. */
    s->R[b] = UVAL_STRAND_MAKE(child);

    /* DORMANT → READY. */
    urbi_strand_start(child);

    return 0;
}

/* ===================================================================
 * Public: OP_JOIN_WAIT
 * =================================================================== */

int
op_join_wait(UStrand *s, UVM *vm, uint32_t instr)
{
    uint8_t a = uinstr_a(instr);   /* child-handle register */
    UStrand *child;

    URBI_ASSERT_NOT_ISR(vm);

    if (s->R[a].kind != (uint8_t)UVAL_STRAND) {
        vm->last_error = UVM_TYPE_ERROR;
        vm_format_type_error_msg(vm,
            "OP_JOIN_WAIT: register operand is not a strand handle");
        s->fatal_status     = UEXEC_CANCEL;
        s->fatal_value.kind = (uint8_t)UVAL_NIL;
        s->fatal_value.v.i  = 0;
        s->state            = USTRAND_STATE_DEAD;
        return -1;
    }
    child = UVAL_AS_STRAND(s->R[a]);

    /* Fast path: child already terminated. */
    if (USTRAND_GET_STATE(child) == USTRAND_DEAD) {
        return 0;   /* parent continues immediately */
    }

    /* Advance the parent's PC past OP_JOIN_WAIT BEFORE blocking.  After
     * fork_wake_joiners wakes us, the child is guaranteed DEAD; we must
     * NOT re-execute OP_JOIN_WAIT on resume because the eager DEAD-strand
     * reap in urbi_step (ustep.c) frees the child once it transitions to
     * DEAD — re-dereferencing R[A] then would read freed memory (S42-FU
     * task #21).  Advancing here ensures the wake path resumes at the
     * instruction AFTER OP_JOIN_WAIT, never re-reading the stale handle.
     *
     * Block parent: thread onto child->joiners_head via wait_next.
     * wait_next is otherwise only used when the parent is on a sleep queue
     * (REASON_SLEEP); here we repurpose it for the join-chain.  This is
     * safe because a strand cannot be simultaneously sleep-blocked AND
     * join-blocked.
     *
     * T31 / VM-004: block-then-link ordering.  Previously the parent was
     * linked onto child->joiners_head BEFORE sched_strand_block, which
     * meant a concurrent walker (e.g. an unwind walker reaching DEAD on
     * the child between the two writes) could observe the parent on the
     * join chain while its state was still RUNNING.  Block first so the
     * parent's state transitions to WAITING|JOIN before it becomes
     * reachable via the join chain. */
    s->pc++;
    sched_strand_block(s, USTRAND_REASON_JOIN, (uint64_t)(uintptr_t)child);
    s->wait_next         = child->joiners_head;
    child->joiners_head  = s;

    /* Signal caller to goto exit_strand. */
    return 1;
}

/* ===================================================================
 * Public: fork_wake_joiners
 * =================================================================== */

void
fork_wake_joiners(UStrand *s, UVM *vm)
{
    UStrand *joiner;
    UStrand *next;

    URBI_ASSERT_NOT_ISR(vm);
    (void)vm;  /* suppress -Wunused-parameter in non-debug builds */

    joiner = s->joiners_head;
    s->joiners_head = NULL;  /* clear before walking so re-entrant calls are no-ops */

    while (joiner != NULL) {
        next = joiner->wait_next;
        joiner->wait_next = NULL;
        /* Wake the joiner so the scheduler dispatches it on the next step. */
        sched_strand_make_runnable(joiner);
        joiner = next;
    }
}
