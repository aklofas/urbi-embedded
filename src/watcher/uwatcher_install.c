/* SPDX-License-Identifier: BSD-3-Clause */
/* install_watcher_runtime: high-level watcher install entry point.
 * Spec #2 §7.1–§7.2.
 *
 * T34: skeleton — re-entry guard + result enum.
 * T35: resolve_owning_tag — cleanup-stack walk.
 * T36: OP_GETSLOT trace probe arm (phase 2+3).
 * T37: run cond on scratch frame + overflow/fault routing (phase 3+4).
 * T38: pool alloc + initialize watcher fields (spec #2 §7.4–§7.5).
 * T39: read-set copy + bit-6 mark + linked-list insertion (spec #2 §7.6).
 * T40: WAITUNTIL strand-block or immediate-wake fast path (spec #2 §7.7). */

#include "watcher/uwatcher_install.h"
#include "watcher/uwatcher.h"   /* UWATCHER_AT, UWatcher, uwatcher_pool_alloc */
#include "uvm.h"                /* UVM, URBI_LOG_WARN */
#include "ustrand.h"            /* UStrand, USTRAND_WAIT_WATCHER */
#include "uclosure.h"           /* UClosure full definition — next_alloc field for closure_list unlink */
#include "uvalue.h"             /* uvalue_truthy (T40) */
#include "ucleanup.h"           /* UCleanupEntry, UCLEANUP_TAG_SCOPE */
#include "realm/urealm.h"       /* URealm — needed for s->realm->tag */
#include "urbi/urbi.h"          /* URBI_LOG_WARN */
#include "umacros.h"            /* URBI_INTERNAL_ASSERT */
#include "gc/ugc_incremental.h" /* UGC_IS_FIXED, UGC_HAS_WATCHER_OBSERVER */
#include "gc/ugc.h"             /* UCell */
#include "utag.h"               /* UTag, member_watchers_head */
#include "uevent.h"             /* UEvent */
#include "uevent_subscribe.h"   /* uevent_at_watchers_append */

/* === strand_closure_unlink ===
 *
 * Remove `cl` from `s->closure_list` (pointer-to-pointer walk) AND detach
 * `cl->proto` from `s->module->nested[]` (by nulling that slot) so that
 * umodule_destroy does not free the proto when the install run ends.
 *
 * Returns 1 if `cl` was found and removed (it was heap-allocated by OP_CLOSURE
 * and both the closure and its proto are now owned by the watcher); returns 0
 * otherwise (NULL pointer, test sentinel, or already unlinked).
 *
 * Called by install_watcher_runtime / install_at_event_runtime to transfer
 * ownership of condition/body/onleave closures from the strand's pre-GC
 * free-list to the watcher.  After a successful unlink:
 *   - uvm_run's closure cleanup loop will not free `cl`
 *   - umodule_destroy will not free `cl->proto` or its sub-buffers
 *   - pool_free must free both proto (+ sub-buffers) and the closure */
static int
strand_closure_unlink(struct UStrand *s, struct UClosure *cl)
{
    struct UClosure **pp;
    size_t k;
    if (cl == NULL) return 0;
    pp = &s->closure_list;
    while (*pp != NULL) {
        if (*pp == cl) {
            *pp = cl->next_alloc;
            cl->next_alloc = NULL;
            /* Detach proto from module->nested[] so umodule_destroy skips it.
             * cl->proto == module->nested[k] for some k; null it out.
             * Graceful if not found (e.g. proto is the root chunk, not nested). */
            if (s->module != NULL && cl->proto != NULL) {
                for (k = 0; k < s->module->nested_count; k++) {
                    if (s->module->nested[k] == cl->proto) {
                        s->module->nested[k] = NULL;
                        break;
                    }
                }
            }
            return 1;   /* found and removed */
        }
        pp = &(*pp)->next_alloc;
    }
    return 0;   /* not found — test sentinel or already unlinked */
}

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
 * Evaluate `cond` on the VM scratch frame and capture the result value +
 * a throw flag.  Test hook short-circuits the dispatch path so existing
 * install-trace tests can inject specific cond results without going
 * through real bytecode dispatch; otherwise routes to
 * urbi_run_closure_on_scratch (uwatcher_scratch.c). */
static void
run_closure_on_scratch_frame_with_result(struct UVM *vm,
                                         struct UClosure *cond,
                                         UValue *out_result,
                                         int    *out_threw)
{
    if (vm->test_install_cond_hook != NULL) {
        vm->test_install_cond_hook(vm, cond, out_result, out_threw);
        return;
    }

    /* M5-proper path: real bytecode dispatch on a transient scratch-frame
     * strand (helper in src/watcher/uwatcher_scratch.c).  The OP_GETSLOT
     * trace probe (armed by install_watcher_runtime via vm->in_watcher_install)
     * records reads into vm->trace_read_set during this dispatch. */
    (void)urbi_run_closure_on_scratch(vm, cond, out_result, out_threw);
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

    /* Ownership transfer: unlink cond/body/onleave from s->closure_list so
     * uvm_run's post-run cleanup loop does not free them.  Only closures that
     * were heap-allocated by OP_CLOSURE will be found on the list; test
     * sentinels ((UClosure *)1 etc.) are not on the list and are not freed.
     * Per-closure ownership bits track which were actually unlinked so
     * pool_free knows exactly which to free on unregister. */
    if (strand_closure_unlink(s, cond))    w->flags |= URBI_WATCHER_OWNS_COND;
    if (strand_closure_unlink(s, body))    w->flags |= URBI_WATCHER_OWNS_BODY;
    if (strand_closure_unlink(s, onleave)) w->flags |= URBI_WATCHER_OWNS_ONLEAVE;

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

    /* WAITUNTIL strand-block (spec #2 §7.7):
     *
     * If cond evaluates truthy at install time, the rising edge IS the install
     * moment — unregister immediately and let the strand continue to the next
     * instruction (fast path / immediate wake).
     *
     * Otherwise, park the waiter strand by transitioning it to WAITING with
     * USTRAND_WAIT_WATCHER reason (0x32).  The OP_WAITUNTIL_INSTALL dispatcher
     * (T42) observes the WAITING state and yields to the scheduler.  The
     * eval-pass wake (T43) will resume the strand when the rising edge fires. */
    if (mode == UWATCHER_WAITUNTIL) {
        if (uvalue_truthy(&cond_value)) {
            /* Immediate wake: unregister the just-installed watcher and let
             * the strand fall through to the next instruction. */
            urbi_watcher_unregister_internal(vm, w);
            return URBI_INSTALL_OK;
        }
        /* Park waiter strand until the rising edge fires (T43 wires wake). */
        s->state = USTRAND_WAIT_WATCHER;
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
            vm->host_log_fn(vm, URBI_LOG_WARN,
                "watcher pool exhausted; AT_EVENT install dropped");
        return URBI_INSTALL_OOM_POOL;
    }

    w->type_tag         = UTYPE_WATCHER;
    w->gc_byte          = (uint8_t)(vm->current_white | UGC_IS_FIXED);
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

    /* Ownership transfer: same pattern as install_watcher_runtime. */
    if (strand_closure_unlink(s, body))    w->flags |= URBI_WATCHER_OWNS_BODY;
    if (strand_closure_unlink(s, onleave)) w->flags |= URBI_WATCHER_OWNS_ONLEAVE;

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

    /* AT_EVENT does NOT join active_watchers_head — only cond watchers walk
     * there.  No watcher_active_count bump needed either; the count tracks
     * cond-watcher pressure on the dirty-eval loop. */

    return URBI_INSTALL_OK;
}
