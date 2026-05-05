/* SPDX-License-Identifier: BSD-3-Clause */
/* install_watcher_runtime: high-level entry point for OP_AT_INSTALL,
 * OP_WHENEVER_INSTALL, and OP_WAITUNTIL_INSTALL dispatchers.
 * Spec #2 §7.1–§7.2.
 *
 * The install path is split across T34–T39:
 *   T34: skeleton + re-entry guard (this file).
 *   T35: resolve_owning_tag (cleanup-stack walk).
 *   T36–T39: trace eval, alloc, read-set wire, and list insert. */

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
    URBI_INSTALL_OK           = 0,
    URBI_INSTALL_OOM_POOL     = 1,
    URBI_INSTALL_READSET_OVER = 2,
    URBI_INSTALL_TRACE_FAULT  = 3,
    URBI_INSTALL_RECURSIVE    = 4,
} UWatcherInstallResult;

/* === install_watcher_runtime ===
 *
 * High-level entry point called by OP_AT_INSTALL / OP_WHENEVER_INSTALL /
 * OP_WAITUNTIL_INSTALL dispatchers.
 *
 * Parameters:
 *   vm      — owning VM.
 *   s       — installing strand (ambient tag scope walked here for T35).
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
 * logged via vm->host_log_fn (if non-NULL). */

UWatcherInstallResult install_watcher_runtime(
    struct UVM     *vm,
    struct UStrand *s,
    uint8_t         mode,
    struct UClosure *cond,
    struct UClosure *body,
    struct UClosure *onleave,
    struct UStrand  *waiter);

#ifdef __cplusplus
}
#endif

#endif /* UWATCHER_INSTALL_H */
