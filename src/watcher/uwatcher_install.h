/* SPDX-License-Identifier: BSD-3-Clause */
/* install_watcher_runtime: high-level entry point for OP_AT_INSTALL,
 * OP_WHENEVER_INSTALL, and OP_WAITUNTIL_INSTALL dispatchers.
 * Spec #2 §7.1–§7.2 (reactive runtime landed in M5; see
 * docs/milestones/m5-reactive.md).
 *
 * Implementation phases live inline in install_watcher_runtime
 * (uwatcher_install.c): re-entry guard, resolve_owning_tag, trace probe arm,
 * cond eval on scratch frame, pool alloc, read-set wire, list insert. */

#ifndef UWATCHER_INSTALL_H
#define UWATCHER_INSTALL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* === Forward declarations === */

struct UVM;
struct UStrand;
struct UClosure;
struct UEvent;

/* === UWatcherInstallResult ===
 *
 * Returned by install_watcher_runtime to its opcode-dispatcher caller.
 * Values are stable — opcodes may branch on them.
 *
 *   OK           — watcher allocated, read-set traced, inserted into active list.
 *   OOM_POOL     — watcher pool exhausted; caller should log and continue.
 *   READSET_OVER — trace overflow; watcher installed without read-set (fires on
 *                  any slot write — conservative but correct).
 *   TRACE_FAULT  — condition closure evaluation raised an error during trace.
 *   RECURSIVE    — install called from within scratch-frame eval (re-entrancy
 *                  guard fired); watcher NOT installed. */

typedef enum {
    URBI_INSTALL_OK                  = 0,
    URBI_INSTALL_OOM_POOL            = 1,
    URBI_INSTALL_READSET_OVER        = 2,
    URBI_INSTALL_TRACE_FAULT         = 3,
    URBI_INSTALL_RECURSIVE           = 4,
    URBI_INSTALL_NO_OBSERVABLE_CELLS = 5,  /* W0/v0.10.2: cond observes no cells;
                                             * rejected as programming error (was
                                             * warn-and-proceed).  Use whenever (e?)
                                             * for event subscriptions. */
} UWatcherInstallResult;

/* === install_watcher_runtime ===
 *
 * High-level entry point called by OP_AT_INSTALL / OP_WHENEVER_INSTALL /
 * OP_WAITUNTIL_INSTALL dispatchers.
 *
 * Parameters:
 *   vm      — owning VM.
 *   s       — installing strand (ambient tag scope walked here via
 *              resolve_owning_tag).
 *   mode    — UWATCHER_AT / UWATCHER_WHENEVER / UWATCHER_WAITUNTIL.
 *   cond    — condition closure; non-NULL for AT/WHENEVER; NULL for WAITUNTIL.
 *   body    — body closure; non-NULL for AT/WHENEVER; NULL for WAITUNTIL.
 *   onleave — onleave closure; may be NULL.
 *   waiter  — blocking strand for WAITUNTIL; NULL for AT/WHENEVER.
 *
 * Returns a UWatcherInstallResult indicating outcome.
 * On URBI_INSTALL_OK the watcher is live and will be evaluated on the
 * next watcher_eval_dirty pass.
 * On any error result no watcher is installed; the error is already
 * logged via vm->host_log_fn (if non-NULL).
 *
 * WAITUNTIL contract (spec #2 §7.7):
 *   - Caller passes cond closure + waiter strand.  Body / onleave are NULL.
 *   - install enters with `s->state == USTRAND_RUNNING` (dispatch context).
 *   - If cond evaluates truthy at install time: the rising edge IS the install
 *     moment.  install unregisters the just-allocated watcher inline and
 *     returns OK with the strand still RUNNING; dispatch falls through to
 *     the next instruction (immediate-wake fast path; WATCH-012/-013).
 *   - Otherwise: install parks the strand at `USTRAND_WAIT_WATCHER` and
 *     returns OK; OP_WAITUNTIL_INSTALL observes WAITING and yields. */

UWatcherInstallResult install_watcher_runtime(
    struct UVM     *vm,
    struct UStrand *s,
    uint8_t         mode,
    struct UClosure *cond,
    struct UClosure *body,
    struct UClosure *onleave,
    struct UStrand  *waiter);

/* install_at_event_runtime: thinner sibling of install_watcher_runtime for
 * AT_EVENT / AT_EVENT_SYNC opcodes.  No read-set trace (events fire on emit,
 * not on slot writes); no active_watchers_head linkage.  Watcher joins
 * event->at_watchers_head + owning_tag's member chain.
 * Spec #3 §6.2. */
UWatcherInstallResult install_at_event_runtime(
    struct UVM     *vm,
    struct UStrand *s,
    uint8_t         mode,
    struct UEvent  *e,
    struct UClosure *body,
    struct UClosure *onleave);

#ifdef __cplusplus
}
#endif

#endif /* UWATCHER_INSTALL_H */
