/* SPDX-License-Identifier: BSD-3-Clause */
/* Chunk-execution C API wrappers (row 8 §5 / T16).
 *
 * M3-baseline note: urbi_run_chunk wraps uvm_run for synchronous execution.
 * The step-driven architecture described in §5 of the chunk-lifecycle spec
 * (per-realm strands, budget loops, T20 strand C API) is deferred to T20.
 * At that point, urbi_run_chunk will route through urbi_step with a real
 * per-realm strand.  This M3 shim preserves the same external contract:
 * blocks until the module OP_RETs, returns URBI_OK on success.
 *
 * Freestanding discipline: no <stdlib.h>, <string.h>, or <assert.h>.
 * String helpers are implemented as byte loops below (same pattern as uemit.c). */

#include "urbi/urbi.h"
#include "realm/urealm.h"
#include "uvm.h"
#include "umodule.h"
#include "uarena.h"
#include "uast.h"
#include "uemit.h"
#include "ulex.h"
#include "uparse.h"
#include "uvalue.h"
#include "object/umoduleinstance.h"
#include <stddef.h>    /* size_t */

/* Freestanding-safe byte-copy: copy at most (cap-1) bytes from src into dst,
 * always NUL-terminates dst when cap > 0.  Mirrors the pattern in uemit.c. */
static void
chunk_strncpy(char *dst, const char *src, size_t cap)
{
    if (cap == 0) return;
    size_t i = 0;
    while (i < cap - 1u && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ---------------------------------------------------------------------------
 * urbi_run_chunk
 *
 * Run a module's root chunk under realm, returning the RET value in
 * *out_result (or discarding it if out_result is NULL).  realm == NULL
 * auto-creates/uses the VM's global Realm.
 *
 * M3 baseline: delegates to uvm_run, which allocates a transient strand and
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

    /* M4 follow-up: bind UModuleInstance so OP_GETSLOT/SETSLOT find IC table.
     * Cache lookup on vm->module_instances_head; lazy create.  OOM here is
     * not fatal — uvm_run will surface a clean diagnostic on first GETSLOT
     * if the binding never happened. */
    (void)urbi_get_or_create_module_instance(vm, (UModule *)module);

    UValue local_out;
    UValue *out = out_result ? out_result : &local_out;

    /* M3 baseline: uvm_run drives a transient strand to completion synchronously.
     * The realm is accepted for API-stability but not yet used to partition
     * bindings — that wiring lands at T20 with the full strand lifecycle API.
     * Suppressing unused-variable warning for realm: it is intentionally held
     * for future use and not yet threaded through uvm_run. */
    (void)realm;

    UVMError rc = uvm_run(vm, module, out);

    switch (rc) {
    case UVM_OK:         return URBI_OK;
    case UVM_OOM:        return URBI_ERR_OOM;
    default:             return URBI_ERR_STRAND_FATAL;
    }
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
 * Mirrors the lex→parse→emit→uvm_run pipeline in tests/unit/test_vm.c.
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

    UModule module = {0};

    UEmitter e;
    uemit_init(&e, &module, &arena, vm, NULL);

    UParser p;
    uparse_init(&p, &lex, &arena);

    bool has_error = false;
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) {
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
            chunk_strncpy(out_buf, "compile error", out_buf_size);
        }
        umodule_destroy(&module);
        uarena_destroy(&arena);
        return URBI_ERR_COMPILE;
    }

    /* Run via urbi_run_chunk (which delegates to uvm_run at M3). */
    UValue result = {0};
    int run_rc = urbi_run_chunk(vm, realm, &module, &result);

    if (run_rc != URBI_OK) {
        /* Copy vm->last_errmsg into out_buf; it was populated by uvm_run. */
        if (out_buf && out_buf_size > 0) {
            chunk_strncpy(out_buf, vm->last_errmsg, out_buf_size);
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
