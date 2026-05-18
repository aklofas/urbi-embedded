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
#include "module/uchunk.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "module/umodule.h"
#include "object/umodule_instance.h"  /* urbi_get_or_create_module_instance */
#include "value/uvalue.h"
#include "runtime/umacros.h"   /* urbi_strncpy_truncating, urbi_zero */
#include "runtime/uclosure.h"  /* UClosure — full struct for vm->stdlib_closures walk */
#include "sched/ustrand.h"     /* UStrand, USTRAND_IS_WAITING, USTRAND_GET_STATE, USTRAND_DEAD */
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

/* v0.8.2 bring-up debug.  Remove before tag. */
#define LDBG(s) do {                                                       \
    if (vm && vm->writer_fn) vm->writer_fn(vm->writer_ud, "ld", 2,         \
                                           s, sizeof(s) - 1U, 0);          \
} while (0)

int
uchunk_loader_drive(UVM *vm, UStrand *loader, UValue *out_result)
{
    LDBG("drive enter\n");
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

    LDBG("for-loop entry\n");
    for (uint32_t i = 0; i < URBI_LOADER_OUTER_CAP; i++) {
        /* v0.8.2 bring-up debug: periodic progress tap so we can distinguish
         * SDRAM-slowness from a stuck step.  Remove before tag.  Prints
         * BEFORE urbi_step so a hang inside urbi_step still leaves a
         * pre-call marker on UART. */
        if (vm->writer_fn && (i & 0x1FU) == 0U) {
            char b[40];
            int n = 0;
            const char *digits = "0123456789ABCDEF";
            const char *tag = "pre i=";
            while (tag[n] && n < 6) { b[n] = tag[n]; n++; }
            for (int k = 28; k >= 0; k -= 4) {
                b[n++] = digits[(i >> k) & 0xF];
            }
            b[n++] = '\r'; b[n++] = '\n';
            vm->writer_fn(vm->writer_ud, "ld", 2, b, (size_t)n, 0);
        }
        UStepResult step_rc = urbi_step(vm, URBI_LOADER_INNER_BUDGET, NULL);
        if (vm->writer_fn && (i & 0x1FU) == 0U) {
            char b[20];
            int n = 0;
            const char *tag = "post rc=";
            while (tag[n] && n < 8) { b[n] = tag[n]; n++; }
            b[n++] = '0' + (int)step_rc;
            b[n++] = '\r'; b[n++] = '\n';
            vm->writer_fn(vm->writer_ud, "ld", 2, b, (size_t)n, 0);
        }

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
            /* v0.8.1 Phase 2: strand-bind ref is on root_proto.
             * Discharge early here via the deferred-destroy-aware helper;
             * null both fields so ustrand_destroy (via urealm_teardown_all)
             * does not double-dec. */
            if (loader->root_proto != NULL) {
                umodule_strand_refcount_dec((UModule *)loader->module,
                                           loader->root_proto, vm);
                loader->root_proto = NULL;
            }
            loader->module = NULL;
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
         * WAITING_SLEEP, WAIT_WATCHER, WAIT_EVENT, WAITING_JOIN, WAITING_HOST. */
        if (USTRAND_IS_WAITING(loader)) {
            /* Parked.  Strand persists in realm; caller continues with
             * their own urbi_step loop.  out_result stays nil. */
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
urbi_run_chunk(UVM *vm, URealm *realm, UModule *module, UValue *out_result)
{
    URBI_ASSERT_NOT_ISR(vm);
    LDBG("run_chunk enter\n");

    if (!realm) {
        realm = urbi_realm_global(vm);
        if (!realm) return URBI_ERR_OOM;
    }

    /* Empty module (instr_count == 0): nothing to dispatch.  Mirrors the
     * urbi_vm_run fast-path (uvm_run.c:44-47) so callers that compile an
     * empty REPL line still get URBI_OK + nil result.  Without this guard,
     * urbi_strand_create_for_module would return NULL (per its precondition)
     * and be misreported as OOM. */
    if (!module || module->root_proto == NULL || module->root_proto->instr_count == 0) {
        LDBG("empty module\n");
        if (out_result) {
            urbi_zero(out_result, sizeof(*out_result));
            out_result->kind = UVAL_NIL;
        }
        return URBI_OK;
    }

    UValue local_out;
    UValue *out = out_result ? out_result : &local_out;

    LDBG("strand_create_for_module...\n");
    UStrand *loader = urbi_strand_create_for_module(vm, realm, module);
    if (!loader) {
        LDBG("strand_create NULL\n");
        vm->last_error = UVM_OOM;
        return URBI_ERR_OOM;
    }
    LDBG("strand_create OK\n");

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

    /* Resolve realm. */
    if (!realm) {
        realm = urbi_realm_global(vm);
        if (!realm) return URBI_ERR_OOM;
    }

    /* Silence out_buf if caller passes zero capacity. */
    if (out_buf && out_buf_size > 0)
        out_buf[0] = '\0';

    /* lex → parse → emit pipeline (inline; no urbi_compile public API yet). */
    ULexer lex;
    ulex_init(&lex, line, line_len);

    UArena arena;
    uarena_init(&arena, 4096);

    /* CHSTR-003: use explicit zero-init via urbi_zero rather than = {0} to
     * document that the module must be fully zero-initialised before uemit_init
     * populates every field.  urbi_zero is the canonical pattern for this. */
    UModule module;
    urbi_zero(&module, sizeof(module));

    UEmitter e;
    uemit_init(&e, &module, &arena, vm, NULL);

    UParser p;
    uparse_init(&p, &lex, &arena);

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
        if (out_buf && out_buf_size > 0) {
#if __STDC_HOSTED__
            if (parse_errmsg && (parse_err_line > 0 || parse_err_col > 0)) {
                /* Full format "stdin:line:col: message" matches compile_source. */
                snprintf(out_buf, out_buf_size, "<stdin>:%d:%d: %s",
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
        umodule_destroy(&module, vm);
        uarena_destroy(&arena);
        return (finish_rc == EMIT_OOM) ? URBI_ERR_OOM : URBI_ERR_COMPILE;
    }

    /* Run the module's root chunk via the persistent loader strand path. */
    UValue result = {0};
    int run_rc = urbi_run_chunk(vm, realm, &module, &result);

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
     * uproto_root_of at vm_alloc_closure time; umodule_destroy rescues the
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
            if (out_buf && out_buf_size > 0)
                uvalue_format(&result, out_buf, out_buf_size);
            umodule_destroy(&module, vm);
            uarena_destroy(&arena);
            return URBI_OK;
        }
        /* Copy vm->last_errmsg into out_buf; it was populated by the driver. */
        if (out_buf && out_buf_size > 0) {
            urbi_strncpy_truncating(out_buf, out_buf_size, vm->last_errmsg);
        }
        umodule_destroy(&module, vm);
        uarena_destroy(&arena);
        return run_rc;
    }

    /* Format the result value into out_buf (nil for fatal, OP_RET value
     * for clean death). */
    if (out_buf && out_buf_size > 0)
        uvalue_format(&result, out_buf, out_buf_size);

    umodule_destroy(&module, vm);
    uarena_destroy(&arena);
    return URBI_OK;
#else
    /* Freestanding: the REPL is not part of the embedded surface.  Mirrors
     * urbi_compile_source's freestanding stub in src/urbi.c — embedders
     * deliver pre-compiled bytecode via urbi_load_module + urbi_run_chunk
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
urbi_run_script(UVM *vm, URealm *realm, UModule *module)
{
    URBI_ASSERT_NOT_ISR(vm);
    return urbi_run_chunk(vm, realm, module, NULL);
}

/* ---------------------------------------------------------------------------
 * urbi_load_module
 *
 * Bind a pre-compiled UModule into the VM and run its root chunk under the
 * global Realm so any top-level bindings install into realm globals.
 *
 * v0.6.0 (API-021): the body was previously a stub that returned a fixed
 * URBI_ERR_INVALID_ARG.  It now performs the minimum useful work that a
 * "load" semantic permits without an import table:
 *
 *   1. Validate (vm, module, module_name) all non-NULL.
 *   2. Bind a UModuleInstance via urbi_get_or_create_module_instance — this
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
 * "v1.x: import-table registration for urbi_load_module".
 *
 * Phase 3 / API-005: when this surface eventually deserializes bytecode it
 * must translate the internal UModuleLoadError ULOAD_UNSUPPORTED_VERSION
 * into the public URBI_ERR_BYTECODE_VERSION_MISMATCH (slot -4 in the
 * UErrCode enum).  See urbi_load_translate_load_err() below — the helper
 * is in place so any future deserialize-bytes entry point routes through
 * a single mapping site.
 * --------------------------------------------------------------------------- */

/* urbi_load_translate_load_err: public-API translation of internal
 * UModuleLoadError → UErrCode.  Closes API-005: ULOAD_UNSUPPORTED_VERSION
 * is now reachable from public callers as URBI_ERR_BYTECODE_VERSION_MISMATCH.
 *
 * Other internal codes collapse to URBI_ERR_INVALID_ARG since the public
 * surface does not yet differentiate them; M6 may grow per-code mappings
 * as the loader API matures. */
int
urbi_load_translate_load_err(int load_err)
{
    if (load_err == 0) return URBI_OK;
    if (load_err == (int)ULOAD_UNSUPPORTED_VERSION) {
        return URBI_ERR_BYTECODE_VERSION_MISMATCH;
    }
    return URBI_ERR_INVALID_ARG;
}

/* ---------------------------------------------------------------------------
 * urbi_module_from_bytes / urbi_module_free  (v0.7.1 spec amendment)
 *
 * Public thin wrappers around umodule_deserialize / umodule_destroy.
 * These exist so the aux layer (urbi_aux_load_and_run) can deserialize
 * bytecode without including internal headers — aux governance requires
 * that aux functions use only the public <urbi/urbi.h> surface.
 *
 * urbi_module_from_bytes:
 *   Heap-allocates a UModule, calls umodule_deserialize on buf[0..len),
 *   and returns the pointer on success.  On failure returns NULL and
 *   writes a diagnostic into errmsg if non-NULL.
 *
 * urbi_module_free:
 *   Calls umodule_destroy (frees all owned allocations) then frees the
 *   UModule itself.  NULL is a no-op.
 * --------------------------------------------------------------------------- */

#if __STDC_HOSTED__
#  include <stdlib.h>   /* malloc, free */
#endif

struct UModule *
urbi_module_from_bytes(const uint8_t *buf, size_t len,
                       char *errmsg, size_t errcap)
{
#if __STDC_HOSTED__
    if (buf == NULL || len == 0) {
        if (errmsg && errcap > 0) {
            errmsg[0] = '\0';
        }
        return NULL;
    }
    UModule *m = (UModule *)malloc(sizeof(UModule));
    if (m == NULL) {
        if (errmsg && errcap > 0) {
            errmsg[0] = '\0';
        }
        return NULL;
    }
    /* zero-init: umodule_deserialize requires a clean UModule */
    {
        size_t i;
        unsigned char *p = (unsigned char *)m;
        for (i = 0; i < sizeof(UModule); i++) p[i] = 0;
    }
    char local_err[256] = {0};
    char *ebuf = errmsg ? errmsg : local_err;
    size_t ecap = errmsg ? errcap : sizeof(local_err);
    UModuleLoadError lerr = umodule_deserialize(m, buf, len, ebuf, ecap);
    if (lerr != ULOAD_OK) {
        /* Task 11: root_proto may have been partially allocated by
         * umodule_deserialize before the error.  umodule_destroy frees
         * root_proto and its buffers; then free the UModule shell. */
        umodule_destroy(m, NULL);
        free(m);
        return NULL;
    }
    return m;
#else
    /* Freestanding: not available — callers on bare-metal manage UModule
     * lifetime themselves (static allocation + umodule_deserialize). */
    (void)buf; (void)len; (void)errmsg; (void)errcap;
    return NULL;
#endif
}

void
urbi_module_free(struct UModule *module)
{
#if __STDC_HOSTED__
    if (module == NULL) return;
    /* v0.8.1 Phase 2: strand-bind refcount is on root_proto.  Assert no live
     * strand binding remains — a nonzero root_proto->refcount means the caller
     * freed the module while strands still hold it (UAF). */
    URBI_INTERNAL_ASSERT((module->root_proto == NULL ||
                          module->root_proto->refcount == 0) &&
        "urbi_module_free called with live strand bindings — call umodule_destroy"
        " + let strands drop refs first");
    /* Public API: no vm in scope.  Pass NULL — no proto rescue path.
     * If a closure has captured a proto from this module, the caller has
     * a lifetime bug regardless of what umodule_destroy does. */
    umodule_destroy(module, NULL);
    free(module);
#else
    (void)module;
#endif
}

int
urbi_load_module(UVM *vm, UModule *module, const char *module_name)
{
    URBI_ASSERT_NOT_ISR(vm);

    if (vm == NULL || module == NULL || module_name == NULL) {
        return URBI_ERR_INVALID_ARG;
    }

    /* Bind a UModuleInstance.  urbi_run_chunk would do this anyway; doing
     * it explicitly here lets us surface OOM as URBI_ERR_OOM rather than
     * conflate it with a runtime-side STRAND_FATAL.  module_name is not
     * stored on the instance at v0.6.0 — it is reserved for v1.x's
     * import-table registration step. */
    if (urbi_get_or_create_module_instance(vm, module) == NULL) {
        return URBI_ERR_OOM;
    }

    return urbi_run_script(vm, NULL, module);
}
