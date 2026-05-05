/* SPDX-License-Identifier: BSD-3-Clause */
/* install_watcher_runtime: high-level watcher install entry point.
 * Spec #2 §7.1–§7.2.
 *
 * T34: skeleton — re-entry guard + result enum.
 * T35: resolve_owning_tag — cleanup-stack walk.
 * T36: OP_GETSLOT trace probe arm (phase 2+3).
 * T37: run cond on scratch frame + overflow/fault routing (phase 3+4).
 * T38: pool alloc + initialize watcher fields (spec #2 §7.4–§7.5).
 * T39: read-set copy + bit-6 mark + linked-list insertion (spec #2 §7.6). */

#include "watcher/uwatcher_install.h"
#include "watcher/uwatcher.h"   /* UWATCHER_AT, UWatcher, uwatcher_pool_alloc */
#include "uvm.h"                /* UVM, URBI_LOG_WARN */
#include "ustrand.h"            /* UStrand */
#include "ucleanup.h"           /* UCleanupEntry, UCLEANUP_TAG_SCOPE */
#include "realm/urealm.h"       /* URealm — needed for s->realm->tag */
#include "urbi/urbi.h"          /* URBI_LOG_WARN */
#include "umacros.h"            /* URBI_INTERNAL_ASSERT */
#include "gc/ugc_incremental.h" /* UGC_IS_FIXED, UGC_HAS_WATCHER_OBSERVER */
#include "gc/ugc.h"             /* UCell */
#include "utag.h"               /* UTag, member_watchers_head */

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

/* === run_closure_on_scratch_frame_with_result (spec #2 §7.3 phase 3) ===
 *
 * Evaluate `cond` on the VM scratch frame and return the result value and
 * a throw flag.
 *
 * M5 stub contract (T37):
 *   1. If vm->test_install_cond_hook != NULL: delegate to hook; hook populates
 *      *out_result and *out_threw.
 *   2. Else: *out_result = UVAL_NIL, *out_threw = 0 (graceful degradation;
 *      watcher installed with a real condition closure but no hook simply yields
 *      nil — watcher never fires by edge, fires immediately by level on first pass).
 *
 * M5 proper: replace this stub with real dispatch_loop_until_yield execution
 * on vm->watcher_scratch_frame, capturing the OP_RET value and setting
 * out_threw on any unhandled THROW/TAG_STOP unwind. */
static void
run_closure_on_scratch_frame_with_result(struct UVM *vm,
                                         struct UClosure *cond,
                                         UValue *out_result,
                                         int    *out_threw)
{
    UValue nil = {0};

    if (vm->test_install_cond_hook != NULL) {
        vm->test_install_cond_hook(vm, cond, out_result, out_threw);
        return;
    }

    *out_result = nil;
    *out_threw  = 0;
}

/* === install_watcher_runtime (spec #2 §7.1) ===
 *
 * T34 skeleton: re-entry guard only.
 * T35: resolve_owning_tag — cleanup-stack walk.
 * T36: phase 2 trace arm.
 * T37: phase 3 cond run + phase 4 overflow/fault routing.
 * T38–T39: pool alloc + linked-list insert. */

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
    if (vm->in_watcher_eval) {
        if (vm->host_log_fn)
            vm->host_log_fn(vm, URBI_LOG_WARN,
                "watcher install attempted from within scratch-frame eval");
        return URBI_INSTALL_RECURSIVE;
    }

    /* Safety: install must not be re-entered from the install path itself. */
    URBI_INTERNAL_ASSERT(vm->in_watcher_install == 0);

    /* Phase 2 (spec #2 §7.3): arm the OP_GETSLOT read-set trace.
     * in_watcher_install gates the UNLIKELY probe in the dispatch loop.
     * trace_overflow and trace_read_set_count are reset here so the probe
     * starts collecting from a clean slate for this install invocation. */
    vm->in_watcher_install   = 1;
    vm->trace_overflow       = 0;
    vm->trace_read_set_count = 0;

    /* Phase 3 (spec #2 §7.3): run cond on scratch frame.
     * The OP_GETSLOT probe (installed in T36) records reads into
     * vm->trace_read_set while in_watcher_install is set. */
    (void)run_closure_on_scratch_frame_with_result(vm, cond,
                                                   &cond_value, &cond_threw);
    vm->in_watcher_install = 0;

    /* Phase 4 (spec #2 §7.3): check overflow and cond-throw. */
    if (vm->trace_overflow) {
        vm->trace_overflow = 0;
        if (vm->host_log_fn)
            vm->host_log_fn(vm, URBI_LOG_WARN,
                "watcher install: read-set exceeds URBI_WATCHER_READSET_MAX");
        return URBI_INSTALL_READSET_OVER;
    }
    if (cond_threw) {
        if (vm->host_log_fn)
            vm->host_log_fn(vm, URBI_LOG_WARN,
                "watcher install: condition threw during trace");
        return URBI_INSTALL_TRACE_FAULT;
    }

    /* Phase 5a (spec #2 §7.4): warn on empty read-set, then proceed.
     * An inert watcher (one that never fires) is introspectable and can be
     * stopped via its owning tag — no surprise no-op. */
    if (vm->trace_read_set_count == 0) {
        if (vm->host_log_fn)
            vm->host_log_fn(vm, URBI_LOG_WARN,
                "watcher condition references no observable cells; will never fire");
        /* Install proceeds — watcher is inert but introspectable. */
    }

    /* Phase 5b (spec #2 §7.4): pool-alloc the watcher record. */
    w = uwatcher_pool_alloc(vm);
    if (w == NULL) {
        if (vm->host_log_fn)
            vm->host_log_fn(vm, URBI_LOG_WARN,
                "watcher install: pool exhausted");
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

    /* Phase 5d (spec #2 §7.6): copy read-set cells + mark bit-6.
     * UGC_HAS_WATCHER_OBSERVER (bit 6) on each cell causes the slot-write
     * barrier to bump vm->watcher_dirty_count on any write to that cell. */
    {
        size_t ri;
        for (ri = 0; ri < (size_t)vm->trace_read_set_count; ri++) {
            UCell *c = vm->trace_read_set[ri];
            c->gc_byte |= UGC_HAS_WATCHER_OBSERVER;
            w->cells[ri] = c;
        }
    }

    /* Phase 5e (spec #2 §7.6): tail-append to active and tag member lists.
     * Tail-append preserves FIFO registration order (row 12 §3.2 contract). */
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

    vm->watcher_active_count++;

    /* WAITUNTIL strand-block arrives in T40; non-WAITUNTIL returns OK here. */
    if (mode == UWATCHER_WAITUNTIL) {
        /* T40: park waiter_strand. For now, fall through to OK. */
        (void)0;
    }

    return URBI_INSTALL_OK;
}
