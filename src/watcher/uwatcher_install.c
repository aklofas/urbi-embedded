/* SPDX-License-Identifier: BSD-3-Clause */
/* install_watcher_runtime: high-level watcher install entry point.
 * Spec #2 §7.1–§7.2 (reactive runtime landed in M5; see
 * docs/milestones/m5-reactive.md).
 *
 * The install path runs as a single linear sequence within
 * install_watcher_runtime:
 *   - re-entry guard + result enum (UWatcherInstallResult).
 *   - resolve_owning_tag — cleanup-stack walk.
 *   - OP_GETSLOT trace probe arm (spec §7.3 phase 2).
 *   - run cond on scratch frame + overflow/fault routing (spec §7.3 phases 3–4).
 *   - pool alloc + initialize watcher fields (spec §7.4–§7.5).
 *   - read-set copy + bit-6 mark + linked-list insertion (spec §7.6).
 *   - WAITUNTIL strand-block or immediate-wake fast path (spec §7.7). */

#include "watcher/uwatcher_install.h"
#include "watcher/uwatcher.h"   /* UWATCHER_AT, UWatcher, uwatcher_pool_alloc */
#include "runtime/uscratch.h"
#include "vm/uvm.h"                /* UVM, URBI_LOG_WARN */
#include "sched/ustrand.h"            /* UStrand, USTRAND_WAIT_WATCHER */
#include "sched/usched_cooperative.h" /* urbi_sched_strand_block (WAITUNTIL park) */
#include "runtime/uclosure.h"           /* UClosure full definition — function parameter types */
#include "value/uvalue.h"             /* uvalue_truthy (WAITUNTIL fast-path test) */
#include "runtime/ucleanup.h"           /* UCleanupEntry, UCLEANUP_TAG_SCOPE */
#include "realm/urealm.h"       /* URealm — needed for s->realm->tag */
#include "urbi/urbi.h"          /* URBI_LOG_WARN */
#include "runtime/umacros.h"            /* URBI_INTERNAL_ASSERT */
#include "gc/ugc_incremental.h" /* UGC_HAS_WATCHER_OBSERVER */
#include "gc/ugc.h"             /* UCell */
#include "tag/utag.h"               /* UTag, member_watchers_head */
#include "event/uevent.h"             /* UEvent */
#include "event/uevent_subscribe.h"   /* uevent_at_watchers_append */
#include <stddef.h>
#include <stdint.h>

/* === resolve_owning_tag (spec #2 §7.2) ===
 *
 * Walk the strand's cleanup stack top-down looking for the innermost
 * UCLEANUP_TAG_SCOPE entry.  Returns its owning_tag pointer.
 * If no TAG_SCOPE entry is found, falls through to s->realm->tag.
 *
 * Bounded by URBI_CLEANUP_MAX (16 footprint / 64 default) — the stack
 * has a fixed pre-allocated capacity so this loop always terminates.
 *
 * Declared non-static so unit tests in test_resolve_owning_tag.c can
 * call it via an extern declaration.  Not exposed in any public header. */

struct UTag *resolve_owning_tag(struct UStrand *s)
{
    int i;
    for (i = (int)s->cleanup_depth - 1; i >= 0; i--) {
        UCleanupEntry *e = &s->cleanup_base[i];
        if ((uint8_t)e->kind == (uint8_t)UCLEANUP_TAG_SCOPE)
            return e->owning_tag;
    }
    /* Fallback: realm tag, or NULL if the strand has no realm (e.g. stack-init
     * in tests or internal strands outside a realm). */
    if (s->realm == NULL) return NULL;
    return s->realm->tag;
}

/* === install_watcher_runtime (spec #2 §7.1) ===
 *
 * High-level watcher install entry point.  Phases (in body order):
 *   1. Re-entry guard.
 *   2. resolve_owning_tag — cleanup-stack walk.
 *   3. Phase 2 (spec §7.3): OP_GETSLOT trace-probe arm.
 *   4. Phase 3 (spec §7.3): cond run on scratch frame + phase-4
 *      overflow/fault routing.
 *   5. Phases 5a–5e (spec §7.4–§7.6): pool alloc + linked-list insert.
 *   6. WAITUNTIL strand-block or immediate-wake fast path (spec §7.7). */

UWatcherInstallResult
install_watcher_runtime(
    struct UVM     *vm,
    struct UStrand *s,
    uint8_t         mode,
    struct UClosure *cond,
    struct UClosure *body,
    struct UClosure *onleave,
    struct UStrand  *waiter)
{
    UValue    cond_value = {0};
    int       cond_threw = 0;
    UWatcher *w;

    /* Re-entry guard: reject install from within scratch-frame eval.
     * This catches the case where a watcher condition closure itself
     * attempts to install a new watcher — which would corrupt the
     * in-progress trace state (spec #2 §7.1 note on recursive install). */
    if (vm->watchers->in_eval) {
        if (vm->host_log_fn)
            vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
                "watcher install attempted from within scratch-frame eval");
        return URBI_INSTALL_RECURSIVE;
    }
    URBI_TP(vm, URBI_TRACE_WATCHER, URBI_LOG_INFO, URBI_TP_WATCHER_INSTALL,
            (uint32_t)mode, 0);
    URBI_PERF_INC(vm, watcher_installs);

    /* Safety: install must not be re-entered from the install path itself. */
    URBI_INTERNAL_ASSERT(vm->watchers->in_install == 0);

    /* Phase 2 (spec #2 §7.3): arm the OP_GETSLOT read-set trace.
     * in_watcher_install gates the UNLIKELY probe in the dispatch loop.
     * trace_overflow and trace_read_set_count are reset here so the probe
     * starts collecting from a clean slate for this install invocation. */
    vm->watchers->in_install   = 1;
    vm->trace_overflow       = 0;
    vm->trace_read_set_count = 0;

    /* Phase 3 (spec #2 §7.3): run cond on scratch frame.
     * Test hook short-circuits the dispatch path so install-trace tests can
     * inject specific cond results; otherwise routes to
     * urbi_run_closure_on_scratch (src/runtime/uscratch.c).  The OP_GETSLOT
     * probe (armed above via in_watcher_install) records reads into
     * vm->trace_read_set during this dispatch. */
    if (vm->test_hooks != NULL && vm->test_hooks->install_cond != NULL) {
        vm->test_hooks->install_cond(vm, cond, &cond_value, &cond_threw);
    } else {
        (void)urbi_run_closure_on_scratch(vm, cond, &cond_value, &cond_threw);
    }
    vm->watchers->in_install = 0;

    /* Phase 4 (spec #2 §7.3): check overflow and cond-throw. */
    if (vm->trace_overflow) {
        vm->trace_overflow = 0;
        if (vm->host_log_fn)
            vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
                "watcher install: read-set exceeds URBI_WATCHER_READSET_MAX");
        return URBI_INSTALL_READSET_OVER;
    }
    if (cond_threw) {
        if (vm->host_log_fn)
            vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
                "watcher install: condition threw during trace");
        return URBI_INSTALL_TRACE_FAULT;
    }

    /* Phase 5a (W0/v0.10.2): empty read-set is a programming error for
     * AT/WHENEVER watchers — reject.  Prior behavior was warn-and-proceed
     * (inert watcher never fires), which silently no-op'd whenever (e?)
     * because parse_whenever previously built AST_WATCHER and the event
     * expression resolved no observable cells.  Now whenever (e?) routes to
     * OP_WHENEVER_EVENT_INSTALL (not here), so any AT/WHENEVER AST_WATCHER
     * reaching install with an empty read-set is either a parse bug or a
     * legacy bytecode using a now-rejected pattern.  Fail loud.
     *
     * WAITUNTIL is exempt: its cond may observe no cells when it evaluates
     * truthy immediately (the Phase 6 immediate-wake fast path unregisters
     * the watcher before it needs to observe any cells).  Closes reactive F1. */
    if (vm->trace_read_set_count == 0
        && mode != UWATCHER_WAITUNTIL) {
        if (vm->host_log_fn)
            vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
                "watcher install rejected: condition has no observable cells "
                "(if intent was event subscription, use `whenever (e?)`)");
        vm->trace_overflow       = 0;
        vm->trace_read_set_count = 0;
        return URBI_INSTALL_NO_OBSERVABLE_CELLS;
    }

    /* Phase 5b (spec #2 §7.4): pool-alloc the watcher record. */
    w = uwatcher_pool_alloc(vm);
    if (w == NULL) {
        if (vm->host_log_fn)
            vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
                "watcher install: pool exhausted");
        /* WATCH-005: clear trace state on fall-through.  Phase 2 reset
         * trace_overflow + trace_read_set_count on entry, but Phase 3
         * populated trace_read_set_count via the OP_GETSLOT probe; the
         * OOM_POOL return must not leak that count to any unrelated
         * reader.  (The next install attempt re-resets at lines 149-150,
         * but defensive cleanup at the fall-through site keeps the
         * invariant local to this function.) */
        vm->trace_overflow       = 0;
        vm->trace_read_set_count = 0;
        return URBI_INSTALL_OOM_POOL;
    }

    /* Phase 5c (spec #2 §7.5): initialize watcher fields.
     * type_tag and gc_byte are set by uwatcher_pool_alloc.
     * exhaust_policy defaults to URBI_EXHAUST_QUEUE (0) from pool_alloc;
     * no VM-level default field exists yet — overwrite for clarity. */
    w->mode             = mode;
    w->exhaust_policy   = URBI_EXHAUST_QUEUE;
    w->flags            = URBI_WATCHER_ACTIVE;
    w->read_set_count   = (uint8_t)vm->trace_read_set_count;
    w->owning_tag       = resolve_owning_tag(s);
    w->realm            = s->realm;
    w->condition        = cond;
    w->body             = body;
    w->onleave          = onleave;
    w->waiter_strand    = waiter;
    w->last_value_cache = cond_value;
    w->body_strand      = NULL;

    /* v0.8.4 Step C-3: strand_closure_unlink + URBI_WATCHER_OWNS_* deleted.
     * UClosure lifetime is GC-managed; watcher fields are GC roots via the
     * walk_uwatcher tracer.  No ownership transfer needed at install time. */

    /* Phase 5d (spec #2 §7.6): copy read-set cells + mark bit-6.
     * UGC_HAS_WATCHER_OBSERVER (bit 6) on each cell causes the slot-write
     * barrier to bump vm->watchers->dirty_count on any write to that cell. */
    {
        size_t ri;
        for (ri = 0; ri < (size_t)vm->trace_read_set_count; ri++) {
            UCell *c = vm->trace_read_set[ri];
            c->gc_byte |= UGC_HAS_WATCHER_OBSERVER;
            w->cells[ri] = c;
        }
    }

    /* Phase 5e (spec #2 §7.6): tail-append to active and tag member lists.
     * Tail-append preserves FIFO registration order (per the determinism
     * contract — install order must equal eval order). */
    w->next_active = NULL;
    if (vm->active_watchers_head == NULL) {
        vm->active_watchers_head = w;
    } else {
        UWatcher *tail = vm->active_watchers_head;
        while (tail->next_active != NULL) tail = tail->next_active;
        tail->next_active = w;
    }

    w->next_in_tag = NULL;
    if (w->owning_tag != NULL) {
        UWatcher *tail = w->owning_tag->member_watchers_head;
        if (tail == NULL) {
            w->owning_tag->member_watchers_head = w;
        } else {
            while (tail->next_in_tag != NULL) tail = tail->next_in_tag;
            tail->next_in_tag = w;
        }
    }

    vm->watchers->active_count++;

    /* WAITUNTIL strand-block (spec #2 §7.7):
     *
     * If cond evaluates truthy at install time, the rising edge IS the install
     * moment — unregister immediately and let the strand continue to the next
     * instruction (fast path / immediate wake).
     *
     * Otherwise, park the waiter strand by transitioning it to WAITING with
     * USTRAND_WAIT_WATCHER reason (0x32) via urbi_sched_strand_block (refactor-3
     * SCHED-01: block owns the runnable-count decrement; the pre-refactor
     * direct state stamp left the accounting to a manual decrement in the
     * OP_WAITUNTIL_INSTALL arm).  The dispatcher observes the WAITING state
     * and yields to the scheduler; the watcher eval pass (urbi_vm_watcher_eval_dirty)
     * wakes the strand by calling urbi_sched_strand_make_runnable when the rising
     * edge fires. */
    if (mode == UWATCHER_WAITUNTIL) {
        if (uvalue_truthy(&cond_value)) {
            /* Immediate wake: unregister the just-installed watcher and let
             * the strand fall through to the next instruction.
             *
             * WATCH-013 (v0.5.7): assert the documented contract — install
             * was entered while the strand is RUNNING (the OP_WAITUNTIL_INSTALL
             * dispatch context), so the immediate-wake path must leave it
             * RUNNING so dispatch resumes at the next instruction.  Without
             * this assertion, a future caller change could pre-park the
             * strand and silently drop the wake intent. */
            URBI_INTERNAL_ASSERT(s->state == USTRAND_RUNNING);
            urbi_watcher_unregister_internal(vm, w);
            return URBI_INSTALL_OK;
        }
        /* Park waiter strand until the rising edge fires; urbi_vm_watcher_eval_dirty
         * wakes it via urbi_sched_strand_make_runnable.  The watcher holds the
         * back-pointer (w->waiter_strand, wired above), so no payload. */
        urbi_sched_strand_block(s, USTRAND_REASON_WATCHER, 0);
    }

    return URBI_INSTALL_OK;
}

/* === install_at_event_runtime (spec #3 §6.2) ===
 *
 * Thinner sibling of install_watcher_runtime for AT_EVENT / AT_EVENT_SYNC.
 * No read-set trace (events fire on emit, not on slot writes).
 * No active_watchers_head linkage — only cond watchers walk there.
 * Watcher joins event->at_watchers_head (FIFO) + owning_tag's member chain.
 * Pool exhaustion is fail-soft: log a warning and return OOM. */

UWatcherInstallResult
install_at_event_runtime(
    struct UVM     *vm,
    struct UStrand *s,
    uint8_t         mode,
    struct UEvent  *e,
    struct UClosure *body,
    struct UClosure *onleave)
{
    UValue nil = {0};
    UWatcher *w = uwatcher_pool_alloc(vm);
    if (!w) {
        if (vm->host_log_fn)
            vm->host_log_fn(vm, vm->host_log_ud, URBI_LOG_WARN,
                "watcher pool exhausted; AT_EVENT install dropped");
        return URBI_INSTALL_OOM_POOL;
    }

    /* type_tag and gc_byte are set by uwatcher_pool_alloc — no re-init needed. */
    w->mode             = mode;
    w->condition        = NULL;  /* no condition closure for event watchers */
    w->body             = body;
    w->onleave          = onleave;
    w->event            = e;
    w->next_in_event    = NULL;
    w->next_in_tag      = NULL;
    w->owning_tag       = resolve_owning_tag(s);
    w->realm            = s->realm;
    w->exhaust_policy   = URBI_EXHAUST_QUEUE;
    w->flags            = URBI_WATCHER_ACTIVE;
    w->body_strand      = NULL;
    w->waiter_strand    = NULL;
    w->last_value_cache = nil;
    w->read_set_count   = 0;

    /* Step C-3: strand_closure_unlink + OWNS_* deleted; GC manages lifetime. */

    uevent_at_watchers_append(e, w);

    if (w->owning_tag) {
        UWatcher *tail = w->owning_tag->member_watchers_head;
        if (tail == NULL) {
            w->owning_tag->member_watchers_head = w;
        } else {
            while (tail->next_in_tag != NULL) tail = tail->next_in_tag;
            tail->next_in_tag = w;
        }
    }

    /* AT_EVENT watchers do NOT join active_watchers_head — only cond
     * watchers walk there (the dirty-eval loop).  But the count covers ALL
     * armed watchers (refactor-3 SCHED-06): it is liveness/reporting
     * bookkeeping (urbi_vm_liveness `armed`, urbi_vm_has_live_work), NOT the
     * eval-list length.  Pre-fix the bump was skipped here while
     * urbi_watcher_unregister_internal decrements unconditionally — uint32
     * underflow, so a VM that ever hosted an event watcher never reported
     * quiescent again. */
    vm->watchers->active_count++;

    return URBI_INSTALL_OK;
}
