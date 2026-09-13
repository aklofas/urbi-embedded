/* SPDX-License-Identifier: BSD-3-Clause */
/* Chunk-execution C API wrappers.
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
 */
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
 */
#define URBI_LOADER_INNER_BUDGET   1000U
#define URBI_LOADER_OUTER_CAP      10000U

/* UAF guard: confirm loader is still in realm->strands_head before reading
 * any of its fields.  Eager-reap may have freed the strand even when
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

/*
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
     * a type-error halt (URBI_ERR_STRAND_FATAL) from a pure unhandled throw
     * (last_error stays URBI_OK).  Mirrors the reset at urbi_vm_run entry
     * (uvm_run.c:29); without this reset a stale URBI_ERR_STRAND_FATAL from a
     * prior REPL session would be misread as belonging to this run. */
    vm->last_error = URBI_OK;

    /* Snapshot the realm pointer BEFORE any urbi_step calls.
     *
     * Safety invariant (eager-reap): urbi_step eagerly calls
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
         * The strand itself stays in realm->strands_head; urealm_teardown_all
         * owns the final urbi_strand_destroy. */
        if (step_rc == URBI_STEP_FATAL && vm->fatal_strand == loader) {
            if (out_result) {
                urbi_zero(out_result, sizeof(*out_result));
                out_result->kind = UVAL_NIL;
            }
            capture_uncaught_throw_diag(vm, loader);
            if (loader->root_proto != NULL) {
                uproto_strand_refcount_dec(loader->root_proto, vm);
                loader->root_proto = NULL;
            }
            vm->fatal_strand = NULL;
            /* Split uncaught script throws from non-throw fatals so batch/
             * embedding callers can distinguish the two cases. */
            return (loader->fatal_status == UEXEC_THROW) ? URBI_ERR_UNCAUGHT_THROW
                                                          : URBI_ERR_STRAND_FATAL;
        }

        /* Path 2: clean death — loader was reaped.  Detected by realm-walk
         * (UAF-safe; the strand list is on the realm, not on the strand).
         * Handles the chunk-top-fork case: even when other strands still
         * bind the module (refcount > 0), the loader itself may have died
         * and been freed; the former mod->refcount == 0 check missed this.
         * out_result was already written by OP_RET via out_slot before reap.
         *
         * Check vm->last_error to surface halt_error-path type errors.
         * halt_error sets vm->last_error = URBI_ERR_STRAND_FATAL (or URBI_ERR_OOM) and
         * marks the strand DEAD directly (fatal_status == UEXEC_OK), so
         * urbi_step eagerly reaps it without going through the FATAL path.
         * Without this check, TypeErrors would silently return URBI_OK and
         * show nil instead of the expected "!!! TypeError:" message. */
        if (!strand_still_alive(loader_realm, loader)) {
            switch (vm->last_error) {
            case URBI_OK:          return URBI_OK;
            case URBI_ERR_OOM:         return URBI_ERR_OOM;
            case URBI_ERR_STRAND_FATAL:  return URBI_ERR_STRAND_FATAL;
            }
            return URBI_ERR_STRAND_FATAL;  /* unknown int */
        }

        /* Path 3: check for parked states (strand still alive — safe to read).
         * USTRAND_IS_WAITING checks that the upper nibble equals
         * USTRAND_WAITING (0x30), covering all WAITING sub-states:
         * WAITING_SLEEP, WAIT_WATCHER, WAIT_EVENT, WAITING_JOIN, WAITING_HOST.
         */
        if (USTRAND_IS_WAITING(loader) || USTRAND_IS_SUSPENDED(loader)) {
            /* Parked.  Strand persists in realm; caller continues with
             * their own urbi_step loop.  out_result stays nil.
             */
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
        vm->last_error = URBI_ERR_OOM;
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
    /* Deliberately NULL: a non-NULL module source_name changes RUNTIME
     * error rendering (urbi_vm_diag_write_prefix: "line N:" → "<name>:N:"), a
     * fixture-pinned surface.  Compile diagnostics still unify on
     * "<stdin>" via the formatter/warning-loop fallbacks below, matching
     * ulex_current_source's default for the parse-error path. */
    uemit_init(&e, module, &arena, vm, NULL);

    UParser p;
    uparse_init(&p, &lex, &arena);
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
                snprintf(out_buf, out_buf_size, "%s:%d:%d: %s",
                         ulex_current_source(&lex),
                         parse_err_line, parse_err_col, parse_errmsg);
            } else
            if (finish_rc != EMIT_OK || e.error != EMIT_OK) {
                /* Use the diag buffer for a positioned emit error when
                 * available.  e.error is the emitter's sticky first error —
                 * statement-level failures break the loop before
                 * uemit_finish runs, so finish_rc alone misses them. */
                char diag_msg[256];
                if (!urbi_emit_diag_format_first_error(&e, diag_msg, sizeof diag_msg)) {
                    snprintf(diag_msg, sizeof diag_msg, "emit error: %s",
                             uemit_error_name(e.error != EMIT_OK ? e.error
                                                                 : finish_rc));
                }
                snprintf(out_buf, out_buf_size, "%s", diag_msg);
            } else
#endif
            {
                const char *msg = parse_errmsg
                                ? parse_errmsg
                                : (finish_rc != EMIT_OK ? uemit_error_name(finish_rc)
                                                        : "compile error");
                urbi_strncpy_truncating(out_buf, out_buf_size, msg);
            }
        }
        urbi_emit_diag_free_all(&e);
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

    /* Display any accumulated compile warnings, then free the diag buffer. */
#if __STDC_HOSTED__
    {
        const char *warn_src = uproto_source_name(e.module);
        int di;
        if (warn_src == NULL) warn_src = "<stdin>";
        for (di = 0; di < e.diag_count; di++) {
            if (e.diag_buf[di].level == UEMIT_DIAG_WARN)
                fprintf(stderr, "%s:%d:%d: warning: %s\n", warn_src,
                        e.diag_buf[di].line, e.diag_buf[di].col,
                        e.diag_buf[di].message);
        }
    }
#endif
    urbi_emit_diag_free_all(&e);

    /* Run the module's root chunk via the persistent loader strand path. */
    UValue result = {0};
    int run_rc = urbi_run_chunk(vm, realm, module, &result);

#ifndef URBI_REPL_DRAIN_BUDGET
#  define URBI_REPL_DRAIN_BUDGET 1000
#endif
    {
        int drain;
        for (drain = 0; drain < URBI_REPL_DRAIN_BUDGET && vm->strand_runnable_count > 0; drain++)
            urbi_step(vm, 1000, NULL);
    }

    if (run_rc != URBI_OK) {
        /* REPL recovery for pure scriptlevel fatals (unhandled throw with no
         * system error): vm->last_error == URBI_OK means the strand died from
         * an unhandled urbiscript `throw` that exhausted the cleanup stack,
         * not from a VM type-error or OOM.  Matching legacy transient-strand
         * behaviour: urbi_vm_run returned URBI_OK (ignoring the strand's
         * UEXEC_THROW fatal_status), so the REPL showed nil.  Preserve that
         * REPL recovery contract — show nil, return URBI_OK.
         *
         * System fatals (TypeError, OOM) have vm->last_error != URBI_OK
         * (set via HALT() in uvm.c) and reach the error-display path below. */
        if ((run_rc == URBI_ERR_UNCAUGHT_THROW || run_rc == URBI_ERR_STRAND_FATAL)
                && vm->last_error == URBI_OK) {
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
 *   1. Validate (vm, module, module_name) all non-NULL.
 *   2. Bind a UChunkInstance via urbi_get_or_create_chunk_instance — this
 *      lazy-interns the IC name strings and prepares the per-(vm, module)
 *      runtime IC backing.  Subsequent urbi_run_chunk / urbi_run_script
 *      calls reuse the same instance.
 *   3. Run the root chunk under the global Realm; any `var foo = ...` at
 *      the module's top level lands in realm->global_object's slot table.
 */

/*
 * Other internal codes collapse to URBI_ERR_INVALID_ARG since the public
 * surface does not yet differentiate them; v1.x may grow per-code mappings
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

/*
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

void
urbi_chunk_free(struct UVM *vm, struct UProto *root)
{
#if __STDC_HOSTED__
    if (root == NULL) return;
    /* Assert no live strand bindings remain — a nonzero refcount means the caller
     * freed the root while strands still hold it (UAF). */
    URBI_INTERNAL_ASSERT(root->refcount == 0 &&
        "urbi_chunk_free called with live strand bindings — let strands drop refs first");
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

/*
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

    uchunk_destroy(root, vm);
    return URBI_OK;
}
