/* SPDX-License-Identifier: BSD-3-Clause */
/* Chunk-execution C API wrappers (row 8 §5 / T16).
 *
 * M3-baseline note: urbi_run_chunk wraps urbi_vm_run for synchronous execution.
 * The step-driven architecture described in §5 of the chunk-lifecycle spec
 * (per-realm strands, budget loops, T20 strand C API) is deferred to T20.
 * At that point, urbi_run_chunk will route through urbi_step with a real
 * per-realm strand.  This M3 shim preserves the same external contract:
 * blocks until the module OP_RETs, returns URBI_OK on success.
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * urbi_strncpy_truncating (runtime/umacros.h) is the shared bounded-copy helper. */

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "vm/uvm.h"
#include "module/umodule.h"
#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "value/uvalue.h"
#include "runtime/umacros.h"   /* urbi_strncpy_truncating, urbi_zero */
#include <stddef.h>    /* size_t */

#if __STDC_HOSTED__
#  include <stdio.h>  /* snprintf: used in urbi_repl_eval to format "src:line:col: msg" */
#endif


/* ---------------------------------------------------------------------------
 * urbi_run_chunk
 *
 * Run a module's root chunk under realm, returning the RET value in
 * *out_result (or discarding it if out_result is NULL).  realm == NULL
 * auto-creates/uses the VM's global Realm.
 *
 * M3 baseline: delegates to urbi_vm_run, which allocates a transient strand and
 * drives it to completion synchronously.  T20 promotes this to the step-driven
 * per-realm strand pattern once the strand C API (urbi_strand_create, etc.) lands.
 * --------------------------------------------------------------------------- */
int
urbi_run_chunk(UVM *vm, URealm *realm, UModule *module, UValue *out_result)
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

    /* realm is accepted for API stability but not yet threaded through
     * urbi_vm_run, which takes (vm, module, out) — no realm parameter.
     * Threading requires expanding urbi_vm_run's signature, which is a
     * Wave-5 boundary change (API-004 carries forward).  At v0.5.5 the
     * realm argument's role is contract validation: it must belong to
     * this vm or be NULL.  Wave 5 wires the partitioned-binding path. */
    (void)realm;

    UVMError rc = urbi_vm_run(vm, module, out);

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
                const char *msg = parse_errmsg ? parse_errmsg : "compile error";
                urbi_strncpy_truncating(out_buf, out_buf_size, msg);
            }
        }
        umodule_destroy(&module);
        uarena_destroy(&arena);
        return URBI_ERR_COMPILE;
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

    if (run_rc != URBI_OK) {
        /* Copy vm->last_errmsg into out_buf; it was populated by urbi_vm_run. */
        if (out_buf && out_buf_size > 0) {
            urbi_strncpy_truncating(out_buf, out_buf_size, vm->last_errmsg);
        }
        umodule_destroy(&module);
        uarena_destroy(&arena);
        return run_rc;
    }

    /* Format the result value into out_buf. */
    if (out_buf && out_buf_size > 0)
        uvalue_format(&result, out_buf, out_buf_size);

    umodule_destroy(&module);
    uarena_destroy(&arena);
    return URBI_OK;
}

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
 * TODO(M6): register module in the VM's import table under module_name so
 * that subsequent urbiscript `import module_name` expressions resolve it.
 * Requires urbi_vm_import_register (M6 API surface) which does not exist yet.
 * --------------------------------------------------------------------------- */
int
urbi_load_module(UVM *vm, UModule *module, const char *module_name)
{
    URBI_ASSERT_NOT_ISR(vm);
    (void)vm;
    (void)module;
    (void)module_name;
    return URBI_ERR_INVALID_ARG;
}
