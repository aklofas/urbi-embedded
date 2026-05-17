/* SPDX-License-Identifier: BSD-3-Clause */
/* Chunk-execution C API wrappers.
 *
 * urbi_run_chunk is a synchronous wrapper around urbi_vm_run: it allocates
 * a transient strand, drives it to completion, and returns when the module
 * OP_RETs.  The step-driven cooperative scheduler (urbi_step + per-realm
 * strands) lives alongside this entry point — embedders that need
 * incremental dispatch use urbi_step directly; urbi_run_chunk is the
 * convenience "block until done" path.
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

    /* Snapshot the module pointer BEFORE any urbi_step calls.
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
     *      via module->refcount: the strand held refcount +1; destroy drops
     *      it.  If refcount reaches 0, the strand died.  Assumes the
     *      driver's caller does not hold an independent module ref (which
     *      it doesn't — the caller passes the module to the strand and does
     *      not call umodule_refcount_inc separately).
     *   3. Parked       — strand still alive; reading loader->state is safe.
     *
     * We snapshot loader->module now (before it's freed) so we can poll
     * refcount safely across iterations. */
    UModule *mod = (UModule *)loader->module;

    for (uint32_t i = 0; i < URBI_LOADER_OUTER_CAP; i++) {
        UStepResult step_rc = urbi_step(vm, URBI_LOADER_INNER_BUDGET, NULL);

        /* Path 1: fatal death.  Fatal strands are not reaped by urbi_step;
         * loader is still valid.  vm->fatal_strand points at our strand,
         * distinguishable from an unrelated strand's fatal by address. */
        if (step_rc == URBI_STEP_FATAL && vm->fatal_strand == loader) {
            if (out_result) {
                urbi_zero(out_result, sizeof(*out_result));
                out_result->kind = UVAL_NIL;
            }
            return URBI_ERR_STRAND_FATAL;
        }

        /* Path 2: clean death detected via module refcount drop.
         * The strand held refcount +1; destroy decrements it.
         * After reap, loader is freed — do NOT dereference it. */
        if (mod != NULL && mod->refcount == 0) {
            /* out_result was already written by OP_RET via out_slot before
             * the reap.  Return success. */
            return URBI_OK;
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
 * Delegates to urbi_vm_run, which allocates a transient strand wired to the
 * resolved realm and drives it synchronously to OP_RET.  Embedders that need
 * incremental, budget-bounded dispatch use urbi_strand_create + urbi_step
 * (the per-realm strand C API) instead.
 * --------------------------------------------------------------------------- */
int
urbi_run_chunk(UVM *vm, URealm *realm, const UModule *module, UValue *out_result)
{
    URBI_ASSERT_NOT_ISR(vm);

    /* Resolve realm: NULL → global (auto-create). */
    if (!realm) {
        realm = urbi_realm_global(vm);
        if (!realm) return URBI_ERR_OOM;
    }

    /* CHSTR-008 + CHSTR-027 (T100): the M4-follow-up precreate of UModuleInstance
     * was redundant.  urbi_vm_run unconditionally calls urbi_module_instance_create
     * (uvm_run.c:114) which builds a fresh instance and prepends it to
     * vm->module_instances_head; the precreate's instance was never read on
     * this path (the strand's module_instance is wired from the fresh-create
     * result, not from the cache lookup).  Keeping the precreate left a
     * dead instance head-inserted into the GC-managed list with no useful
     * effect; the GC reaps it on the next sweep but the work is wasted.
     * Removed.  vm->strand_runnable_count and module_instance_count for
     * .chk fixtures unchanged after the removal. */

    UValue local_out;
    UValue *out = out_result ? out_result : &local_out;

    /* API-004 (Wave 5): thread the caller-supplied Realm through to
     * urbi_vm_run — pre-Wave-5 the realm argument was silently dropped
     * via `(void)realm;` and urbi_vm_run always wired the transient to
     * the global Realm.  After Wave 5, urbi_vm_run accepts a realm
     * directly (NULL → global, preserving the prior implicit behavior). */
    UVMError rc = urbi_vm_run(vm, realm, module, out);

    /* Map UVMError to UErrCode.  UVM_TYPE_ERROR collapses to STRAND_FATAL
     * at v0.5.5 because the public surface has no dedicated type-error
     * code; M6 stdlib expansion may add one (API-032 review). */
    switch (rc) {
    case UVM_OK:         return URBI_OK;
    case UVM_OOM:        return URBI_ERR_OOM;
    case UVM_TYPE_ERROR: return URBI_ERR_STRAND_FATAL;
    }
    return URBI_ERR_STRAND_FATAL;  /* unreachable; new UVMError values must add cases */
}

#if !defined(URBI_BYTECODE_ONLY) && __STDC_HOSTED__
/* ---------------------------------------------------------------------------
 * urbi_steal_repl_protos (file-private helper)
 *
 * Before a REPL-session UModule is destroyed, check whether any UClosure on
 * vm->stdlib_closures references a UProto owned by this module.  If so,
 * steal the entire nested[] array:
 *   1. Set module->nested = NULL so umodule_destroy skips the array.
 *   2. Thread each individual UProto onto vm->stdlib_protos so their
 *      instruction buffers (and other owned fields) are freed at
 *      urbi_vm_destroy.
 *   3. Track the array pointer itself via a UNestedArrayNode on
 *      vm->stdlib_nested_arrays so it is freed at urbi_vm_destroy AFTER
 *      the protos.  The UClosures whose origin_nested points at this array
 *      remain valid for their entire lifetime.
 *
 * Root cause this fixes: closures installed as realm-globals (via
 * `var f = function() { ... }` at chunk-top, or setSlot / class-body
 * var-decl) are migrated to vm->stdlib_closures at urbi_vm_run exit.
 * Their UProto objects are owned by the REPL-session UModule.  Without this
 * steal, umodule_destroy frees those protos, leaving dangling proto pointers
 * in every surviving UClosure.  The next cross-session call to such a
 * closure loads dangling instructions and segfaults.
 *
 * Why steal the WHOLE array (not just directly-referenced protos):
 * OP_CLOSURE inside a callee reads the nested[] array indexed by bx to
 * create inner functions.  The callee uses origin_nested[bx] (Phase 5
 * fix), so origin_nested must be the FULL original array — individual
 * entries that weren't yet instantiated as closures must still be present.
 * Stealing individual entries (nulling module->nested[i]) keeps the array
 * allocated but with holes; stealing the whole array avoids any aliasing
 * confusion: the surviving array is the one closures point to.
 *
 * Watcher-detached protos (module->nested[i] == NULL at session end,
 * cleared by the watcher install path) are skipped in the proto thread-on
 * loop; their memory is owned by the watcher pool.
 * --------------------------------------------------------------------------- */
static void
urbi_steal_repl_protos(UVM *vm, UModule *module)
{
    if (module->nested == NULL || module->nested_count == 0) return;
    if (vm->stdlib_closures == NULL) return;

    /* Phase 1: scan to see if any proto in this module is referenced by a
     * stdlib_closure.  One positive match means we must steal the whole
     * nested[] array. */
    int needs_steal = 0;
    for (size_t ni = 0; ni < module->nested_count && !needs_steal; ni++) {
        const UProto *p = module->nested[ni];
        if (p == NULL) continue;
        UClosure *cl = vm->stdlib_closures;
        while (cl != NULL) {
            if (cl->proto == p) { needs_steal = 1; break; }
            cl = cl->next_alloc;
        }
    }
    if (!needs_steal) return;

    /* Phase 2a: allocate a bookkeeping node to track the array pointer.
     * If this allocation fails, fall back to not stealing (proto buffers
     * will be freed by umodule_destroy — dangling closure, but no worse
     * than pre-Phase-5 behaviour, and much less likely than OOM here). */
    UModuleAllocFn arr_alloc = module->alloc_fn
                               ? module->alloc_fn : vm->alloc_fn;
    void          *arr_ud    = module->alloc_fn
                               ? module->alloc_ud : vm->alloc_ud;
    UNestedArrayNode *node = vm->alloc_fn(NULL,
                                          sizeof(UNestedArrayNode),
                                          vm->alloc_ud);
    if (node == NULL) return;  /* OOM — leave module intact */

    /* Phase 2b: steal the array pointer and detach it from the module.
     * umodule_destroy checks module->nested == NULL and skips array free. */
    node->arr      = module->nested;
    node->alloc_fn = arr_alloc;
    node->alloc_ud = arr_ud;
    node->next     = vm->stdlib_nested_arrays;
    vm->stdlib_nested_arrays = node;
    module->nested = NULL;   /* prevents umodule_destroy from freeing it */

    /* Phase 2c: thread individual UProto structs onto vm->stdlib_protos so
     * their owned buffers (instructions, consts, upval_descs) are freed at
     * urbi_vm_destroy.  Watcher-detached slots (NULL entries) are skipped. */
    for (size_t ni = 0; ni < module->nested_count; ni++) {
        UProto *p = node->arr[ni];
        if (p == NULL) continue;  /* watcher-detached — owned by watcher pool */
        p->next_alloc     = vm->stdlib_protos;
        vm->stdlib_protos = p;
    }
}
#endif /* !URBI_BYTECODE_ONLY && __STDC_HOSTED__ */

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

    /* Run via urbi_run_chunk (which delegates to urbi_vm_run at M3). */
    UValue result = {0};
    int run_rc = urbi_run_chunk(vm, realm, &module, &result);

    /* API-009: drain any body strands spawned by watcher eval during this run.
     * urbi_vm_run (inside urbi_run_chunk) only drives its own transient strand;
     * spawned body strands accumulate in vm->ready_head and need urbi_step
     * to execute.  Cap at URBI_REPL_DRAIN_BUDGET iterations to prevent
     * infinite spin with persistent watchers. */
#ifndef URBI_REPL_DRAIN_BUDGET
#  define URBI_REPL_DRAIN_BUDGET 1000
#endif
    {
        int drain;
        for (drain = 0; drain < URBI_REPL_DRAIN_BUDGET && vm->strand_runnable_count > 0; drain++)
            urbi_step(vm, 1000, NULL);
    }

    /* Phase 5 (Gap #1): steal any nested UProtos referenced by stdlib_closures
     * BEFORE umodule_destroy frees them.  Without this, closures installed as
     * realm-globals in this run (and migrated to vm->stdlib_closures at
     * urbi_vm_run exit) would have dangling proto->instructions pointers on
     * the next cross-session call.  See urbi_steal_repl_protos (above) for
     * the full root-cause explanation. */
    urbi_steal_repl_protos(vm, &module);

    if (run_rc != URBI_OK) {
        /* Copy vm->last_errmsg into out_buf; it was populated by urbi_vm_run. */
        if (out_buf && out_buf_size > 0) {
            urbi_strncpy_truncating(out_buf, out_buf_size, vm->last_errmsg);
        }
        umodule_destroy(&module, vm);
        uarena_destroy(&arena);
        return run_rc;
    }

    /* Format the result value into out_buf. */
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
urbi_run_script(UVM *vm, URealm *realm, const UModule *module)
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
    /* v0.8.0: all callers are synchronous (urbi_run_chunk + fail-path teardown);
     * the transient strand's refcount decrement has already fired before we get
     * here.  Assert that no live strand binding remains — a nonzero refcount here
     * means the caller freed the module while strands still hold it (UAF). */
    URBI_INTERNAL_ASSERT(module->refcount == 0 &&
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
