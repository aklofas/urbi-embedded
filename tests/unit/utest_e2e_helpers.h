/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/utest_e2e_helpers.h
 *
 * Shared end-to-end helpers for scripted-install reactive tests.
 *
 * Consumers (Phase 1 of v0.5.8-cleanup):
 *   - test_at_scripted_e2e.c
 *   - test_at_sync_scripted.c
 *   - test_tag_stop_onleave_scripted.c
 *   - test_event_sync_emit_scripted.c (uses the _with_module variant)
 *
 * Each helper is a thin wrapper around the public urbi API + the lex /
 * parse / emit pipeline, factored out from per-file copies that had
 * drifted slightly across the v0.5.0 .. v0.5.7 reactive-runtime landings
 * (REACT-POLISH-001 backlog, docs/urbi-embedded-backlog.md L757).
 *
 * Note: test_uwatcher_scratch.c is sometimes listed as a fifth consumer
 * but its `compile_source` helper is compile-only (it never runs the
 * chunk because the test drives urbi_run_closure_on_scratch directly).
 * That is a different operation, not a near-verbatim copy of these
 * helpers, and is left in place.
 */

#ifndef URBI_TESTS_UTEST_E2E_HELPERS_H
#define URBI_TESTS_UTEST_E2E_HELPERS_H

#include <stdint.h>

#include "urbi/urbi.h"
#include "value/uarena.h"
#include "chunk/uchunk.h"
#include "vm/uvm.h"

/* utest_e2e_compile_and_run: lex + parse + emit + run `src` under the
 * VM's global realm.
 *
 * Returns URBI_OK on success, URBI_ERR_OOM if no global realm,
 * URBI_ERR_COMPILE on a parse / emit error, or whatever urbi_run_chunk
 * returns from execution.  out_result may be NULL (result discarded).
 *
 * Owns its arena and module: both are destroyed before return.  Use
 * utest_e2e_compile_and_run_with_module when the test needs the module
 * (and any returned UVAL_CLOSURE) to outlive the call.
 */
int utest_e2e_compile_and_run(UVM *vm, const char *src, UValue *out_result);

/* utest_e2e_compile_and_run_with_module: same as compile_and_run but the
 * caller supplies and owns the arena and module, so any UVAL_CLOSURE in
 * `*out_result` stays live as long as the caller keeps them alive.
 *
 * Required by tests that capture a function-literal closure and call it
 * back later (e.g., test_event_sync_emit_scripted.c, where the body
 * closure is installed in an AT_EVENT_SYNC watcher and fired via
 * c_event_emit_sync after compile_and_run returns).
 */
int utest_e2e_compile_and_run_with_module(UVM *vm,
                                          UArena *arena,
                                          UModule *module,
                                          const char *src,
                                          UValue *out_result);

/* utest_e2e_run_to_no_runnable: drive the VM until strand_runnable_count
 * reaches 0, a fatal strand is detected, or an iteration cap is hit.
 *
 * Returns 1 on quiescence (or URBI_STEP_WAKE_AT / URBI_STEP_QUIESCENT,
 *                          which both imply no runnable strand),
 *         -1 on URBI_STEP_FATAL,
 *          0 on cap exhaustion (timeout).
 *
 * Cap: 1000 iterations.  Sufficient for every reactive e2e test today;
 * if a future test legitimately needs more, raise the cap here.
 */
int utest_e2e_run_to_no_runnable(UVM *vm);

/* utest_e2e_make_int / utest_e2e_make_nil: small UValue constructors
 * used as expected-value literals in assertions.  make_nil delegates to
 * urbi_make_nil() (introduced in v0.5.7) for canonical zero-init.
 */
UValue utest_e2e_make_int(int64_t n);
UValue utest_e2e_make_nil(void);

#endif /* URBI_TESTS_UTEST_E2E_HELPERS_H */
