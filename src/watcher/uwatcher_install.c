/* SPDX-License-Identifier: BSD-3-Clause */
/* install_watcher_runtime: high-level watcher install entry point.
 * Spec #2 §7.1–§7.2.
 *
 * T34: skeleton — re-entry guard + result enum.
 * T35: resolve_owning_tag — cleanup-stack walk.
 * T36–T39: trace eval, pool alloc, read-set wire, list insert. */

#include "watcher/uwatcher_install.h"
#include "watcher/uwatcher.h"   /* UWATCHER_AT etc. */
#include "uvm.h"                /* UVM, URBI_LOG_WARN */
#include "ustrand.h"            /* UStrand */
#include "ucleanup.h"           /* UCleanupEntry, UCLEANUP_TAG_SCOPE */
#include "realm/urealm.h"       /* URealm — needed for s->realm->tag */
#include "urbi/urbi.h"          /* URBI_LOG_WARN */
#include "umacros.h"            /* URBI_INTERNAL_ASSERT */

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
    return s->realm->tag;
}

/* === install_watcher_runtime (spec #2 §7.1) ===
 *
 * T34 skeleton: re-entry guard only.
 * Trace + alloc + insert land in T35–T39. */

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
    (void)s;
    (void)mode;
    (void)cond;
    (void)body;
    (void)onleave;
    (void)waiter;

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

    /* Trace + alloc + insert: T36–T39. */
    return URBI_INSTALL_OK;
}
