/* SPDX-License-Identifier: BSD-3-Clause */
/* twatcher_install_helper: test-only low-level pool+wiring helper used by
 * unit tests that exercise the watcher pool primitive directly.
 *
 * Background (closes WATCH-023): the function previously lived in production
 * code as `urbi_watcher_install_internal` (src/watcher/uwatcher.c) and was
 * documented as "test-only seam".  In practice no production call site
 * referenced it — `urbi_watcher_install_watcher_runtime` and `urbi_watcher_install_at_event_runtime`
 * inline their own pool-alloc + list-wiring sequence.  Wave 5 retired the
 * production-side declaration and lifted the implementation here so the
 * test scaffolding is no longer part of the public src/ surface.
 *
 * urbi_watcher_install_for_test allocates one watcher slot from the pool,
 * wires the read-set bit-6 + cell pointer copy, tail-inserts onto
 * vm->active_watchers_head, and head-inserts onto owning_tag's
 * member_watchers_head.  Returns NULL on pool exhaustion or read-set
 * overflow.
 *
 * The helper does NOT run real bytecode dispatch on the condition closure.
 * Install-time seeding short-circuits via vm->test_watcher_condition_hook
 * when set; otherwise seeds last_value_cache to nil.  Tests passing real
 * GC-managed closures (urbi_make_native_closure) as condition MUST also set
 * test_watcher_condition_hook before any subsequent eval, since
 * urbi_watcher_invoke_condition_closure dispatches real bytecode (v0.5.1-cond-unstub).
 * Integer-cast sentinel values ((UClosure *)1 etc.) are deprecated; they
 * crash the GC walker if a GC cycle runs (Step D / T17).
 *
 * Cleanup goes through the production unregister entry point
 * urbi_watcher_unregister_internal (still in src/watcher/uwatcher.c) since
 * it is a real production primitive shared with urbi_watcher_install_watcher_runtime. */

#ifndef TWATCHER_INSTALL_HELPER_H
#define TWATCHER_INSTALL_HELPER_H

#include <stddef.h>
#include <stdint.h>

struct UVM;
struct UTag;
struct UCell;
struct UWatcher;
struct UClosure;

struct UWatcher *urbi_watcher_install_for_test(
    struct UVM       *vm,
    uint8_t           mode,
    struct UTag      *owning_tag,
    struct UClosure  *condition,
    struct UClosure  *body,
    struct UClosure  *onleave,
    struct UCell    **read_set,
    size_t            read_set_count);

#endif /* TWATCHER_INSTALL_HELPER_H */
