/* SPDX-License-Identifier: BSD-3-Clause */
/* Chunk-execution C API wrappers.
 *
 * v0.8.0: urbi_run_chunk allocates a persistent loader strand via
 * urbi_strand_create_for_module and drives it via uchunk_loader_drive's
 * park-or-die state machine.  The strand persists in realm->strands_head;
 * the host's main urbi_step loop advances it after urbi_run_chunk returns.
 * This replaces the v0.7.x transient-strand path and enables chunk-top `&`
 * and `,` fork semantics per the legacy urbiscript spec.
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * urbi_strncpy_truncating (runtime/umacros.h) is the shared bounded-copy helper. */

#include "urbi/urbi.h"
#include "chunk/uchunk_strand.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "chunk/uchunk.h"
#include "object/uchunk_instance.h"  /* urbi_get_or_create_chunk_instance */
#include "value/uvalue.h"
#include "runtime/umacros.h"   /* urbi_strncpy_truncating, urbi_zero */
#include "runtime/uclosure.h"  /* UClosure — full struct for closure type usage */
#include "sched/ustrand.h"     /* UStrand, USTRAND_IS_WAITING, USTRAND_GET_STATE, USTRAND_DEAD */
#include "object/uobject.h"    /* UObject, urbi_object_resolve_slot — uncaught-throw diag */
#include "value/uintern.h"     /* ustr_intern — "message" slot lookup */
#include <stddef.h>    /* size_t */
#include <stdint.h>    /* uint32_t */

#if !defined(URBI_BYTECODE_ONLY)
#  include "value/uarena.h"
#  include "parse/uast.h"
#  include "emit/uemit.h"
#  include "lex/ulex.h"
#  include "parse/uparse.h"
#  if __STDC_HOSTED__
#    include <stdio.h>  /* snprintf: used in urbi_repl_eval to format "src:line:col: msg" */
#  endif
#endif


/* ---------------------------------------------------------------------------
 * urealm_register_module
 *
 * Register `m` onto `realm->loaded_protos_head` via head-insertion if not
 * already linked.  Idempotent: a second call with the same module for the
 * same realm is a no-op (owning_realm already set to this realm).
 * Shared modules (e.g. vm->stdlib_module) may be run into multiple realms;
 * they keep owning_realm pointing at the FIRST realm they were registered
 * in and are NOT re-registered for subsequent realms.  The unload path
 * (Task 12 / 13) handles per-realm teardown independently — except for
 * vm_owned overlays (refactor-3 GC-18): realm teardown only clears their
 * back-pointers; urbi_vm_destroy frees them.  v0.9.0-repl. */
static void
urealm_register_module(URealm *realm, UProto *p)
{
    if (realm == NULL || p == NULL) return;
    /* Already registered (in some realm) — skip to avoid double-linking. */
    if (p->owning_realm != NULL) return;
    p->next_in_realm = realm->loaded_protos_head;
    p->owning_realm  = realm;
    realm->loaded_protos_head = p;
}

/* ---------------------------------------------------------------------------
 * uchunk_loader_drive
 *
 * v0.8.0: driver-loop budget for urbi_run_chunk's internal urbi_step
 * iterations.  Inner: per-step instruction budget passed to urbi_step.
 * Outer: how many urbi_step iterations to attempt before giving up
 * (returning URBI_ERR_LOADER_BUDGET).  At 1000 × 10000 = 10M instructions,
 * far beyond any reasonable chunk-top workload; an infinite-loop chunk
 * would hit this cap.  Tunable later if a workload demands it.
 * --------------------------------------------------------------------------- */
#define URBI_LOADER_INNER_BUDGET   1000U
#define URBI_LOADER_OUTER_CAP      10000U

/* UAF guard: confirm loader is still in realm->strands_head before reading
 * any of its fields.  T20 eager-reap may have freed the strand even when
 * mod->refcount > 0 (other strands — e.g. chunk-top fork children — still
 * bind the same module).  Realm-walk is UAF-safe: the list is rooted on
 * the realm (not on the strand), so we never touch freed memory.
 * O(n) in live strands; typical chunk-top workloads have <10 strands. */
static bool
strand_still_alive(const URealm *realm, const UStrand *loader)
{
    if (!realm || !loader) return false;
    for (const UStrand *s = realm->strands_head; s != NULL; s = s->next_in_realm) {
        if (s == loader) return true;
    }
    return false;
}

/* v0.11.4-cat-f (D-F2): capture a backward-compatible diagnostic for an
 * uncaught throw whose value is an Exception-instance object.  Prior tasks
 * made VM-internal errors (TypeError/ArityError/...) catchable as typed
 * Exception instances; when such an instance escapes to the top level the
 * REPL must restore the legacy "!!! <message>" line instead of bare nil.
 *
 * Only UVAL_OBJECT throws produce a diagnostic here — scalar/string throws
 * (`throw 42`, `throw "x"`) keep the historical nil-recovery contract
 * (control_transfer/throw_uncaught.chk).  Writes the instance's `message`
 * slot (a UVAL_STR) into vm->last_errmsg; falls back to a generic label for
 * an object with no string message slot.  Leaves last_errmsg empty for any
 * non-object thrown value so the caller can distinguish the two cases.
 *
 * Reads the dead loader strand's fatal_value while it is still live (Path 1
 * of uchunk_loader_drive — fatal strands are not eager-reaped). */
static void
capture_uncaught_throw_diag(UVM *vm, const UStrand *loader)
{
    if (vm == NULL || loader == NULL) return;
    /* Only THROW fatals reach urbi_repl_eval's nil-recovery block (which keys
     * off an empty last_errmsg).  For non-THROW fatals (e.g. D3 outside-scope
     * tag.stop, which pre-sets last_errmsg via utag_native.c) leave the buffer
     * untouched so its diagnostic survives. */
    if (loader->fatal_status != UEXEC_THROW) return;
    vm->last_errmsg[0] = '\0';
    if (loader->fatal_value.kind != (uint8_t)UVAL_OBJECT) return;

    UObject *e = (UObject *)loader->fatal_value.v.p;
    if (e == NULL) return;

    const char *msg = NULL;
    const USymbol *sym_message = (const USymbol *)ustr_intern(vm, "message", 7);
    if (sym_message != NULL) {
        UObject *holder = NULL;
        uint32_t idx    = 0U;
        int rc = urbi_object_resolve_slot(vm, e, sym_message, &holder, &idx);
        if (rc > 0 && holder != NULL) {
            UValue mv = holder->slots[idx];
            if (mv.kind == (uint8_t)UVAL_STR && mv.v.p != NULL)
                msg = (const char *)mv.v.p;
        }
    }
    urbi_strncpy_truncating(vm->last_errmsg, sizeof vm->last_errmsg,
                            msg != NULL ? msg : "<exception>");
}

int
uchunk_loader_drive(UVM *vm, UStrand *loader, UValue *out_result)
{
    if (!vm || !loader) {
        if (out_result) {
            urbi_zero(out_result, sizeof(*out_result));
            out_result->kind = UVAL_NIL;
        }
        return URBI_ERR_INVALID_ARG;
    }

    /* Wire out_result as the strand's out_slot so OP_RET writes the return
     * value directly.  A local nil is used when the caller passes NULL. */
    UValue nil_storage;
    urbi_zero(&nil_storage, sizeof(nil_storage));
    nil_storage.kind = UVAL_NIL;

    UValue *out_slot_target = out_result ? out_result : &nil_storage;
    /* Initialise to nil; OP_RET will overwrite on clean death. */
    *out_slot_target = nil_storage;
    loader->out_slot = out_slot_target;

    /* Reset vm->last_error so that clean-death detection can distinguish
     * a type-error halt (UVM_TYPE_ERROR) from a pure unhandled throw
     * (last_error stays UVM_OK).  Mirrors the reset at urbi_vm_run entry
     * (uvm_run.c:29); without this reset a stale UVM_TYPE_ERROR from a
     * prior REPL session would be misread as belonging to this run. */
    vm->last_error = UVM_OK;

    /* Snapshot the realm pointer BEFORE any urbi_step calls.
     *
     * Safety invariant (T20 eager-reap): urbi_step eagerly calls
     * urbi_strand_destroy on clean-dead strands, which frees the strand
     * struct.  Reading `loader->state` after urbi_step returns is a UAF
     * if the strand died cleanly.  Fatal strands are NOT reaped (ustep.c
     * returns URBI_STEP_FATAL and sets vm->fatal_strand before the reap
     * arm); those are safe to read.
     *
     * Detection strategy:
     *   1. Fatal death  — urbi_step returns URBI_STEP_FATAL and
     *      vm->fatal_strand == loader.  Strand not freed; still readable.
     *   2. Clean death  — strand was reaped; `loader` is invalid.  Detected
     *      via realm-walk (strand_still_alive): the strand list is rooted on
     *      the realm (not on the freed strand), so the walk never touches
     *      freed memory.  Subsumes the former mod->refcount == 0 heuristic,
     *      which only worked when loader was the LAST strand binding the
     *      module (broke under chunk-top fork: children also hold a ref).
     *   3. Parked       — strand still alive; reading loader->state is safe.
     *
     * We snapshot loader->realm now (before it's freed) so we can call
     * strand_still_alive safely across iterations. */
    const URealm *loader_realm = loader->realm;

    for (uint32_t i = 0; i < URBI_LOADER_OUTER_CAP; i++) {
        UStepResult step_rc = urbi_step(vm, URBI_LOADER_INNER_BUDGET, NULL);

        /* Path 1: fatal death.  Fatal strands are not reaped by urbi_step;
         * loader is still valid.  vm->fatal_strand points at our strand,
         * distinguishable from an unrelated strand's fatal by address.
         *
         * v0.8.0 lifetime contract: urbi_run_chunk may use a stack-allocated
         * UModule (urbi_repl_eval pattern).  The fatal loader strand holds a
         * module refcount that must be discharged BEFORE the caller frees the
         * module (which happens when urbi_repl_eval returns and the stack
         * frame unwinds).  Discharge early here:
         *   1. Drop the module refcount and null s->module so the later
         *      realm-teardown path (urealm_teardown_all → urbi_strand_destroy
         *      → ustrand_destroy) does not double-decrement.
         *   2. Clear vm->fatal_strand so the urbi_step fast-path does not
         *      see a stale pointer on the next host urbi_step call.
         *
         * The strand itself stays in realm->strands_head; urealm_teardown_all
         * owns the final urbi_strand_destroy. */
        if (step_rc == URBI_STEP_FATAL && vm->fatal_strand == loader) {
            if (out_result) {
                urbi_zero(out_result, sizeof(*out_result));
                out_result->kind = UVAL_NIL;
            }
            /* v0.11.4-cat-f (D-F2): capture an uncaught Exception-instance's
             * message into vm->last_errmsg while the loader strand is still
             * live (fatal strands are not eager-reaped).  urbi_repl_eval reads
             * this to restore the legacy "!!! <message>" diagnostic. */
            capture_uncaught_throw_diag(vm, loader);
            /* v0.8.1 Phase 2: strand-bind ref is on root_proto.
             * Discharge early here via the deferred-destroy-aware helper;
             * null root_proto so ustrand_destroy (via urealm_teardown_all)
             * does not double-dec.
             * v0.9.2: loader->module deleted; root_proto is the sole identity. */
            if (loader->root_proto != NULL) {
                uproto_strand_refcount_dec(loader->root_proto, vm);
                loader->root_proto = NULL;
            }
            vm->fatal_strand = NULL;
            return URBI_ERR_STRAND_FATAL;
        }

        /* Path 2: clean death — loader was reaped.  Detected by realm-walk
         * (UAF-safe; the strand list is on the realm, not on the strand).
         * Handles the chunk-top-fork case: even when other strands still
         * bind the module (refcount > 0), the loader itself may have died
         * and been freed; the former mod->refcount == 0 check missed this.
         * out_result was already written by OP_RET via out_slot before reap.
         *
         * Check vm->last_error to surface halt_error-path type errors.
         * halt_error sets vm->last_error = UVM_TYPE_ERROR (or UVM_OOM) and
         * marks the strand DEAD directly (fatal_status == UEXEC_OK), so
         * urbi_step eagerly reaps it without going through the FATAL path.
         * Without this check, TypeErrors would silently return URBI_OK and
         * show nil instead of the expected "!!! TypeError:" message. */
        if (!strand_still_alive(loader_realm, loader)) {
            switch (vm->last_error) {
            case UVM_OK:          return URBI_OK;
            case UVM_OOM:         return URBI_ERR_OOM;
            case UVM_TYPE_ERROR:  return URBI_ERR_STRAND_FATAL;
            }
            return URBI_ERR_STRAND_FATAL;  /* unknown UVMError */
        }

        /* Path 3: check for parked states (strand still alive — safe to read).
         * USTRAND_IS_WAITING checks that the upper nibble equals
         * USTRAND_WAITING (0x30), covering all WAITING sub-states:
         * WAITING_SLEEP, WAIT_WATCHER, WAIT_EVENT, WAITING_JOIN, WAITING_HOST.
         *
         * refactor-3 VM-03: SUSPENDED (0x50) is parked too — a chunk-top
         * t.block()/t.freeze() self-suspend now exits dispatch SUSPENDED
         * (OP_CALL post-native arm).  Without this arm the drive loop
         * classified SUSPENDED as "keep driving", spun the outer cap on a
         * quiescent VM, and returned URBI_ERR_LOADER_BUDGET WITHOUT clearing
         * out_slot — a later urbi_tag_unblock + urbi_step resumed the strand
         * and OP_RET wrote 16 bytes through the dangling pointer into
         * urbi_repl_eval's dead frame (ASan stack-use-after-return). */
        if (USTRAND_IS_WAITING(loader) || USTRAND_IS_SUSPENDED(loader)) {
            /* Parked.  Strand persists in realm; caller continues with
             * their own urbi_step loop.  out_result stays nil.
             *
             * W6/v0.10.2: clear out_slot so OP_RET on resume does not write
             * to a dangling pointer (the caller's UValue result local is on
             * the stack of urbi_repl_eval which has already returned).
             * OP_RET guards against NULL out_slot (uvm.c CASE(OP_RET)), so
             * the strand's return value is silently discarded — the REPL
             * already emitted nil for the parked call. */
            loader->out_slot = NULL;
            return URBI_OK;
        }

        /* Otherwise READY or RUNNING — keep driving. */
    }

    /* Outer cap exhausted: chunk-top still runnable after 10M instructions
     * of forward progress with no yield.  Almost certainly an infinite loop. */
    return URBI_ERR_LOADER_BUDGET;
}

/* ---------------------------------------------------------------------------
 * urbi_run_chunk
 *
 * Run a module's root chunk under realm, returning the RET value in
 * *out_result (or discarding it if out_result is NULL).  realm == NULL
 * auto-creates/uses the VM's global Realm.
 *
 * v0.8.0: persistent loader strand path.  Allocates a real scheduler-managed
 * strand via urbi_strand_create_for_module; the driver runs urbi_step
 * iterations until the strand parks (sleep / join-wait / event-wait) or
 * dies (OP_RET / fatal).
 *
 * On parked: the strand persists in realm->strands_head; the host's main
 * urbi_step loop continues advancing it after this returns.
 * On dead: scheduler reaped the strand; out_slot was written to *out_result
 * via out_slot wiring inside uchunk_loader_drive.
 *
 * module parameter is non-const: urbi_strand_create_for_module bumps
 * module->refcount (a mutating operation).
 * --------------------------------------------------------------------------- */
int
urbi_run_chunk(UVM *vm, URealm *realm, UProto *root, UValue *out_result)
{
    URBI_ASSERT_NOT_ISR(vm);

    if (!realm) {
        realm = urbi_realm_global(vm);
        if (!realm) return URBI_ERR_OOM;
    }

    /* Register root onto realm->loaded_protos_head (idempotent). */
    urealm_register_module(realm, root);

    /* Empty root (instr_count == 0): nothing to dispatch.  Mirrors the
     * urbi_vm_run fast-path (uvm_run.c:44-47) so callers that compile an
     * empty REPL line still get URBI_OK + nil result.  Without this guard,
     * urbi_strand_create_for_module would return NULL (per its precondition)
     * and be misreported as OOM. */
    if (!root || root->instr_count == 0) {
        if (out_result) {
            urbi_zero(out_result, sizeof(*out_result));
            out_result->kind = UVAL_NIL;
        }
        return URBI_OK;
    }

    UValue local_out;
    UValue *out = out_result ? out_result : &local_out;

    UStrand *loader = urbi_strand_create_for_module(vm, realm, root);
    if (!loader) {
        vm->last_error = UVM_OOM;
        return URBI_ERR_OOM;
    }

    return uchunk_loader_drive(vm, loader, out);
}

#if !defined(URBI_BYTECODE_ONLY)
/* ---------------------------------------------------------------------------
 * urbi_repl_eval
 *
 * Compile `line` (source length `line_len`), run it under `realm`, and format
 * the result into `out_buf`.  realm == NULL uses the global Realm.
 *
 * Returns URBI_OK on success (buf has printable result or is empty for void).
 * Returns URBI_ERR_COMPILE on parse/emit error (buf gets "compile error").
 * Returns URBI_ERR_STRAND_FATAL on runtime error (buf gets vm->last_errmsg).
 *
 * Mirrors the lex→parse→emit→urbi_vm_run pipeline in tests/unit/test_vm.c.
 * --------------------------------------------------------------------------- */
int
urbi_repl_eval(UVM *vm, URealm *realm, const char *line, size_t line_len,
               char *out_buf, size_t out_buf_size)
{
#if __STDC_HOSTED__
    URBI_ASSERT_NOT_ISR(vm);
    URBI_TP(vm, URBI_TRACE_REPL, URBI_LOG_INFO, URBI_TP_REPL_EVAL, 1u,
            (uint32_t)line_len);

    /* Resolve realm. */
    if (!realm) {
        realm = urbi_realm_global(vm);
        if (!realm) return URBI_ERR_OOM;
    }

    /* Silence out_buf if caller passes zero capacity. */
    if (out_buf && out_buf_size > 0)
        out_buf[0] = '\0';

    /* v0.9.1 compile-budget: source-byte check fires before any allocation
     * so an oversized submission is rejected without touching the arena or
     * UModule allocator.  Depth + node-count limits are enforced inside
     * the parser via UCompileBudget threaded through UParser. */
    const UCompileBudget *budget = urbi_realm_get_compile_budget(vm, realm);
    if (budget != NULL && budget->max_source_bytes > 0U
            && line_len > (size_t)budget->max_source_bytes) {
        if (out_buf && out_buf_size > 0) {
            urbi_strncpy_truncating(out_buf, out_buf_size,
                "compile-budget exceeded: source bytes");
        }
        return URBI_ERR_COMPILE_BUDGET_SOURCE;
    }

    /* lex → parse → emit pipeline (inline; no urbi_compile public API yet). */
    ULexer lex;
    ulex_init(&lex, line, line_len);

    UArena arena;
    uarena_init(&arena, 4096);

    /* v0.9.0-repl (CHSTR-027): heap-allocate root UProto per REPL line so each
     * root persists in realm->loaded_protos_head past return.  The old
     * stack-alloc reused the same address across REPL lines, causing the
     * realm registry to alias.  Freed by urbi_realm_destroy (session-end)
     * or the root_proto-refcount rescue mechanism (if a strand parks).
     * v0.9.2: root IS the UProto; no UModule shell. */
    UProto *module = (UProto *)vm->alloc_fn(NULL, sizeof(UProto), vm->alloc_ud);
    if (module == NULL) {
        if (out_buf && out_buf_size > 0) {
            urbi_strncpy_truncating(out_buf, out_buf_size, "OOM allocating root proto");
        }
        uarena_destroy(&arena);
        return URBI_ERR_OOM;
    }
    urbi_zero(module, sizeof *module);
    module->alloc_fn = vm->alloc_fn;
    module->alloc_ud = vm->alloc_ud;
    module->heap_allocated = true;

    UEmitter e;
    uemit_init(&e, module, &arena, vm, NULL);

    UParser p;
    uparse_init(&p, &lex, &arena);
    /* v0.9.1: thread the realm's compile-budget (if any) into the parser so
     * depth + node-count limits are enforced as we go.  budget is NULL when
     * the realm has no budget — uparse_set_budget(NULL) is the explicit
     * "unlimited" path and matches the default uparse_init left behind. */
    uparse_set_budget(&p, budget);

    bool has_error = false;
    const char *parse_errmsg = NULL;  /* static message from AST_ERROR node */
    int  parse_err_line = 0, parse_err_col = 0;
    /* Used inside #if __STDC_HOSTED__ snprintf path below; silence
     * cppcheck unreadVariable on freestanding builds. */
    (void)parse_err_line; (void)parse_err_col;
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) {
            parse_errmsg    = node->u.err.message;
            parse_err_line  = node->line;
            parse_err_col   = node->col;
            has_error = true;
            break;
        }
        UEmitError emit_rc = uemit_statement(&e, node);
        if (emit_rc != EMIT_OK) {
            has_error = true;
            break;
        }
        uarena_reset(&arena);
    }

    UEmitError finish_rc = EMIT_OK;
    if (!has_error) {
        finish_rc = uemit_finish(&e);
        if (finish_rc != EMIT_OK)
            has_error = true;
    }

    if (has_error) {
        /* v0.9.1: budget trip surfaces here when uparse_next_statement
         * returned the OOM sentinel (or a NULL from an inner make_node).
         * Latch is sticky in UParser; check before composing the diag. */
        int budget_err = uparse_budget_err(&p);
        if (out_buf && out_buf_size > 0) {
#if __STDC_HOSTED__
            if (budget_err != URBI_OK) {
                /* Pin the specific limit in the message so embedders can
                 * recognise the failure mode from the buffer alone. */
                const char *which =
                    (budget_err == URBI_ERR_COMPILE_BUDGET_DEPTH) ? "depth" :
                    (budget_err == URBI_ERR_COMPILE_BUDGET_NODES) ? "nodes" :
                                                                    "source";
                snprintf(out_buf, out_buf_size,
                         "compile-budget exceeded: %s", which);
            } else
            if (parse_errmsg && (parse_err_line > 0 || parse_err_col > 0)) {
                /* v0.9.0-repl: route lex.source_name through so syncline-framed
                 * REPL submissions show correct file:line in errors. */
                snprintf(out_buf, out_buf_size, "%s:%d:%d: %s",
                         ulex_current_source(&lex),
                         parse_err_line, parse_err_col, parse_errmsg);
            } else
#endif
            {
                /* CPPCHK-005: surface uemit_finish's diagnostic when the parser
                 * succeeded but finalization failed (e.g. EMIT_OOM, constant
                 * pool exhausted at top-level RET emission).  Falls back to
                 * the parser's static message when the parse stage errored. */
                const char *msg = parse_errmsg
                                ? parse_errmsg
                                : (finish_rc != EMIT_OK ? uemit_error_name(finish_rc)
                                                        : "compile error");
                urbi_strncpy_truncating(out_buf, out_buf_size, msg);
            }
        }
        /* Parse / statement-emit errors skipped uemit_finish; release
         * emitter-owned funcstate storage (no-op when has_error came from
         * uemit_finish itself, which already tore it down — FE-07). */
        urbi_emit_abandon(&e);
        /* Compile-error path: module was never registered in the realm
         * (urbi_run_chunk was not reached), so it is not realm-owned.
         * uchunk_destroy frees struct internals and, since heap_allocated=true,
         * frees the UProto itself too. */
        uchunk_destroy(module, vm);
        uarena_destroy(&arena);
        if (budget_err != URBI_OK) return budget_err;
        return (finish_rc == EMIT_OOM) ? URBI_ERR_OOM : URBI_ERR_COMPILE;
    }

    /* Run the module's root chunk via the persistent loader strand path. */
    UValue result = {0};
    int run_rc = urbi_run_chunk(vm, realm, module, &result);

    /* API-009: drain any body strands spawned by watcher eval during this run.
     * urbi_run_chunk now returns when the loader strand parks (persists
     * in realm) or completes.  Body strands spawned by watcher eval still
     * accumulate in the ready queue and need draining.
     * Cap at URBI_REPL_DRAIN_BUDGET iterations to prevent
     * infinite spin with persistent watchers. */
#ifndef URBI_REPL_DRAIN_BUDGET
#  define URBI_REPL_DRAIN_BUDGET 1000
#endif
    {
        int drain;
        for (drain = 0; drain < URBI_REPL_DRAIN_BUDGET && vm->strand_runnable_count > 0; drain++)
            urbi_step(vm, 1000, NULL);
    }

    /* v0.8.1 Variant B Phase 2: urbi_steal_repl_protos deleted.
     * Closures escaping into realm globals bump root_proto.refcount via
     * uproto_root_of at vm_alloc_closure time; uchunk_destroy rescues the
     * entire root_proto when refcount > 0.  No per-nested stealing needed. */

    if (run_rc != URBI_OK) {
        /* REPL recovery for pure scriptlevel fatals (unhandled throw with no
         * system error): vm->last_error == UVM_OK means the strand died from
         * an unhandled urbiscript `throw` that exhausted the cleanup stack,
         * not from a VM type-error or OOM.  Matching legacy transient-strand
         * behaviour: urbi_vm_run returned UVM_OK (ignoring the strand's
         * UEXEC_THROW fatal_status), so the REPL showed nil.  Preserve that
         * REPL recovery contract — show nil, return URBI_OK.
         *
         * System fatals (TypeError, OOM) have vm->last_error != UVM_OK
         * (set via HALT() in uvm.c) and reach the error-display path below. */
        if (run_rc == URBI_ERR_STRAND_FATAL && vm->last_error == UVM_OK) {
            /* v0.11.4-cat-f (D-F2): an uncaught throw whose value is an
             * Exception-instance object left a diagnostic in vm->last_errmsg
             * (capture_uncaught_throw_diag, Path 1).  Restore the legacy
             * "!!! <message>" line by surfacing the message and returning
             * the fatal code (the REPL prints the !!! framing on non-OK).
             * Scalar/string throws leave last_errmsg empty and keep the
             * historical nil-recovery contract below. */
            if (vm->last_errmsg[0] != '\0') {
                if (out_buf && out_buf_size > 0)
                    urbi_strncpy_truncating(out_buf, out_buf_size, vm->last_errmsg);
                uarena_destroy(&arena);
                return URBI_ERR_STRAND_FATAL;
            }
            if (out_buf && out_buf_size > 0)
                uvalue_format(&result, out_buf, out_buf_size);
            /* Module is realm-owned (heap-alloc); do NOT unload here —
             * closures may still reference its protos.  urbi_realm_destroy
             * or the refcount-rescue mechanism handles final cleanup. */
            uarena_destroy(&arena);
            return URBI_OK;
        }
        /* Copy vm->last_errmsg into out_buf; it was populated by the driver. */
        if (out_buf && out_buf_size > 0) {
            urbi_strncpy_truncating(out_buf, out_buf_size, vm->last_errmsg);
        }
        /* Module is realm-owned (heap-alloc); do NOT unload here —
         * closures may still reference its protos.  urbi_realm_destroy
         * handles final cleanup. */
        uarena_destroy(&arena);
        return run_rc;
    }

    /* Format the result value into out_buf (nil for fatal, OP_RET value
     * for clean death). */
    if (out_buf && out_buf_size > 0)
        uvalue_format(&result, out_buf, out_buf_size);

    /* Module is realm-owned (heap-alloc); do NOT unload here — it persists
     * in realm->loaded_protos_head until urbi_realm_destroy or explicit
     * urbi_unload by the host.  This is the CHSTR-027 close-out. */
    uarena_destroy(&arena);
    return URBI_OK;
#else
    /* Freestanding: the REPL is not part of the embedded surface.  Mirrors
     * urbi_compile_source's freestanding stub in src/urbi.c — embedders
     * deliver pre-compiled bytecode via urbi_load_chunk + urbi_run_chunk
     * instead.  uarena_init (the hosted entry point) isn't declared in
     * freestanding mode, so this branch returns early without touching
     * any compiler front-end primitives. */
    (void)vm; (void)realm; (void)line; (void)line_len;
    (void)out_buf; (void)out_buf_size;
    return URBI_ERR_COMPILE;
#endif /* __STDC_HOSTED__ */
}
#endif /* !URBI_BYTECODE_ONLY */

/* ---------------------------------------------------------------------------
 * urbi_run_script
 *
 * Thin wrapper: run a pre-compiled module, discard the result.
 * realm == NULL uses the global Realm.  Per §5, the host is responsible for
 * driving urbi_step() afterwards if the script registered watchers/coroutines.
 * --------------------------------------------------------------------------- */
int
urbi_run_script(UVM *vm, URealm *realm, UProto *root)
{
    URBI_ASSERT_NOT_ISR(vm);
    return urbi_run_chunk(vm, realm, root, NULL);
}

/* ---------------------------------------------------------------------------
 * urbi_load_chunk
 *
 * Bind a pre-compiled UProto chunk into the VM and run its root chunk under
 * the global Realm so any top-level bindings install into realm globals.
 *
 * v0.6.0 (API-021): the body was previously a stub that returned a fixed
 * URBI_ERR_INVALID_ARG.  It now performs the minimum useful work that a
 * "load" semantic permits without an import table:
 *
 *   1. Validate (vm, module, module_name) all non-NULL.
 *   2. Bind a UChunkInstance via urbi_get_or_create_chunk_instance — this
 *      lazy-interns the IC name strings and prepares the per-(vm, module)
 *      runtime IC backing.  Subsequent urbi_run_chunk / urbi_run_script
 *      calls reuse the same instance.
 *   3. Run the root chunk under the global Realm; any `var foo = ...` at
 *      the module's top level lands in realm->global_object's slot table.
 *
 * The module_name argument is currently advisory: with no import table it
 * cannot be looked up via urbiscript `import "name"`.  v1.x adds the
 * import-registry surface and threads module_name through the registration
 * step; the existing public API stays compatible.  See backlog entry
 * "v1.x: import-table registration for urbi_load_chunk".
 *
 * Phase 3 / API-005: when this surface eventually deserializes bytecode it
 * must translate the internal UChunkLoadError UCHUNK_LOAD_UNSUPPORTED_VERSION
 * into the public URBI_ERR_BYTECODE_VERSION_MISMATCH (slot -4 in the
 * UErrCode enum).  See urbi_chunk_translate_load_err() below — the helper
 * is in place so any future deserialize-bytes entry point routes through
 * a single mapping site.
 * --------------------------------------------------------------------------- */

/* urbi_chunk_translate_load_err: public-API translation of internal
 * UChunkLoadError → UErrCode.  Closes API-005: UCHUNK_LOAD_UNSUPPORTED_VERSION
 * is now reachable from public callers as URBI_ERR_BYTECODE_VERSION_MISMATCH.
 *
 * Other internal codes collapse to URBI_ERR_INVALID_ARG since the public
 * surface does not yet differentiate them; M6 may grow per-code mappings
 * as the loader API matures. */
int
urbi_chunk_translate_load_err(int load_err)
{
    if (load_err == 0) return URBI_OK;
    if (load_err == (int)UCHUNK_LOAD_UNSUPPORTED_VERSION) {
        return URBI_ERR_BYTECODE_VERSION_MISMATCH;
    }
    return URBI_ERR_INVALID_ARG;
}

/* ---------------------------------------------------------------------------
 * urbi_chunk_from_bytes / urbi_chunk_free  (v0.7.1 spec amendment)
 *
 * Public thin wrappers around uchunk_deserialize / uchunk_destroy.
 * These exist so the aux layer (urbi_aux_load_and_run) can deserialize
 * bytecode without including internal headers — aux governance requires
 * that aux functions use only the public <urbi/urbi.h> surface.
 *
 * urbi_chunk_from_bytes:
 *   Deserializes buf[0..len) into a heap-allocated root UProto and returns
 *   it on success.  On failure returns NULL and writes a diagnostic into
 *   errmsg if non-NULL.
 *
 * urbi_chunk_free:
 *   Calls uchunk_destroy (frees all owned allocations) then frees the root
 *   UProto itself.  NULL is a no-op.
 * --------------------------------------------------------------------------- */

#if __STDC_HOSTED__
#  include <stdlib.h>   /* malloc, free */
#endif

/* v0.10.3 W5: urbi_chunk_from_bytes gains (struct UVM *vm, ...) as first arg
 * and routes allocation through vm->alloc_fn on hosted builds when vm is
 * non-NULL (closes the cross-allocator hazard in api-ergonomics F3).
 * Falls back to stdlib_alloc when vm is NULL (backward-compat path for
 * tests that call with a dummy vm).
 * Returns root UProto on success. */
struct UProto *
urbi_chunk_from_bytes(struct UVM *vm, const uint8_t *buf, size_t len,
                      char *errmsg, size_t errcap)
{
#if __STDC_HOSTED__
    if (buf == NULL || len == 0) {
        if (errmsg && errcap > 0) errmsg[0] = '\0';
        return NULL;
    }
    char local_err[256] = {0};
    char *ebuf = errmsg ? errmsg : local_err;
    size_t ecap = errmsg ? errcap : sizeof(local_err);
    UProto *root = NULL;
    /* Route through vm->alloc_fn when available (W5 allocator routing).
     * Pass NULL alloc_fn when vm is NULL — uchunk_deserialize uses stdlib_alloc. */
    UVMAllocFn afn = (vm != NULL) ? vm->alloc_fn : NULL;
    void      *aud = (vm != NULL) ? vm->alloc_ud : NULL;
    UChunkLoadError lerr = uchunk_deserialize(&root, buf, len, afn, aud, ebuf, ecap);
    if (lerr != UCHUNK_LOAD_OK) {
        /* uchunk_deserialize frees partial allocations on failure. */
        return NULL;
    }
    return root;
#else
    /* Freestanding: not available — callers on bare-metal use uchunk_deserialize
     * directly with an explicit alloc_fn. */
    (void)vm; (void)buf; (void)len; (void)errmsg; (void)errcap;
    return NULL;
#endif
}

/* v0.10.3 W5: urbi_chunk_free gains (struct UVM *vm, ...) as first arg.
 * vm is used for the alloc_fn on hosted builds to free via the same domain
 * as urbi_chunk_from_bytes.  Falls back to NULL (stdlib free) when vm is NULL. */
void
urbi_chunk_free(struct UVM *vm, struct UProto *root)
{
#if __STDC_HOSTED__
    if (root == NULL) return;
    /* Assert no live strand bindings remain — a nonzero refcount means the caller
     * freed the root while strands still hold it (UAF). */
    URBI_INTERNAL_ASSERT(root->refcount == 0 &&
        "urbi_chunk_free called with live strand bindings — let strands drop refs first");
    /* Use vm's alloc_fn for freeing when available (W5 allocator routing).
     * Pass NULL when vm is absent — uchunk_destroy uses stdlib free on hosted. */
    uchunk_destroy(root, vm);
    /* uchunk_destroy frees the struct when heap_allocated; no separate free needed. */
#else
    (void)vm; (void)root;
#endif
}

int
urbi_load_chunk(UVM *vm, UProto *root, const char *module_name)
{
    URBI_ASSERT_NOT_ISR(vm);

    if (vm == NULL || root == NULL || module_name == NULL) {
        return URBI_ERR_INVALID_ARG;
    }

    /* Bind a UChunkInstance.  urbi_run_chunk would do this anyway; doing
     * it explicitly here lets us surface OOM as URBI_ERR_OOM rather than
     * conflate it with a runtime-side STRAND_FATAL.  module_name is not
     * stored on the instance at v0.6.0 — it is reserved for v1.x's
     * import-table registration step. */
    if (urbi_get_or_create_chunk_instance(vm, root) == NULL) {
        return URBI_ERR_OOM;
    }

    /* Registration happens transitively via urbi_run_chunk (called from
     * urbi_run_script).  Call explicitly here against the global realm so
     * the root is registered even if urbi_run_script short-circuits on an
     * empty root chunk.  Idempotent — second call from run_chunk is a no-op. */
    URealm *global_realm = urbi_realm_global(vm);
    if (global_realm != NULL) {
        urealm_register_module(global_realm, root);
    }

    return urbi_run_script(vm, NULL, root);
}

/* ---------------------------------------------------------------------------
 * urbi_unload  (v0.9.0-repl Task 11)
 *
 * Unlink module from its owning realm's loaded_protos_head list and route
 * through uchunk_destroy.  If root_proto->refcount > 0 the rescue mechanism
 * defers final cleanup; this call returns URBI_OK either way.
 * --------------------------------------------------------------------------- */
int
urbi_unload(UVM *vm, UProto *root)
{
    if (vm == NULL || root == NULL)        return URBI_ERR_INVALID_ARG;
    if (root->owning_realm == NULL)        return URBI_ERR_INVALID_ARG;
    URBI_ASSERT_NOT_ISR(vm);

    URealm *r = root->owning_realm;

    /* Unlink from the realm's loaded_protos_head list. */
    if (r->loaded_protos_head == root) {
        r->loaded_protos_head = root->next_in_realm;
    } else {
        for (UProto *p = r->loaded_protos_head; p != NULL; p = p->next_in_realm) {
            if (p->next_in_realm == root) {
                p->next_in_realm = root->next_in_realm;
                break;
            }
        }
    }
    root->owning_realm  = NULL;
    root->next_in_realm = NULL;

    /* Route through uchunk_destroy.  If refcount > 0, rescue mechanism
     * defers final cleanup; this call always returns success.
     * uchunk_destroy also unlinks any UChunkInstance from
     * vm->module_instances_head, so no dangling cells survive.
     * v0.9.2: uchunk_destroy frees heap_allocated roots automatically. */
    uchunk_destroy(root, vm);
    return URBI_OK;
}
