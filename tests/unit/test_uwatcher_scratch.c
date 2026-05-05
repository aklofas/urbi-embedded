/* SPDX-License-Identifier: BSD-3-Clause */
/* Direct unit tests for urbi_run_closure_on_scratch.
 *
 * Tests compile urbiscript source through the standard lex/parse/emit
 * pipeline, wrap the compiled root chunk in a UClosure, and exercise
 * urbi_run_closure_on_scratch directly.  This approach is resilient to
 * opcode-numbering and pool-encoding changes because the emitter produces
 * the bytecode, not the test.
 *
 * End-to-end scripted-at coverage lives in test_at_scripted_e2e.c (T9). */

#include "utest.h"
#include "uvm.h"
#include "umodule.h"
#include "uclosure.h"
#include "uarena.h"
#include "uemit.h"
#include "ulex.h"
#include "uparse.h"
#include "watcher/uwatcher.h"

#include <stddef.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Compile urbiscript source into `module` (caller-provided, zero-init).
 * Uses the standard lex/parse/emit pipeline.
 * Returns 1 on success, 0 on failure.
 * Caller must call umodule_destroy(&module) when done. */
static int
compile_source(UVM *vm, UArena *arena, UModule *module, const char *src)
{
    ULexer   lex;
    UEmitter e;
    UParser  p;
    UAstNode *node;

    ulex_init(&lex, src, strlen(src));
    uemit_init(&e, module, arena, vm, NULL);
    uparse_init(&p, &lex, arena);

    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) return 0;
        (void)uemit_statement(&e, node);
        uarena_reset(arena);
    }

    return (uemit_finish(&e) == EMIT_OK) ? 1 : 0;
}

/* ===================================================================
 * Test cases
 * =================================================================== */

/* scratch_runner_returns_integer_value
 *
 * Compile "42", wrap the root chunk in a closure, run via
 * urbi_run_closure_on_scratch, and verify the return value is UVAL_INT 42. */
UTEST(scratch_runner_returns_integer_value)
{
    UVM    vm;
    UArena arena;
    UModule module;

    uvm_init(&vm, NULL, NULL);
    uarena_init(&arena, 4096);
    memset(&module, 0, sizeof(module));

    int ok = compile_source(&vm, &arena, &module, "42");
    UASSERT(ok);

    /* Build a stack-local UProto and UClosure wrapping the module's root chunk.
     * The module's instruction/constant arrays stay owned by the module. */
    UProto proto;
    memset(&proto, 0, sizeof(proto));
    proto.instructions = module.instructions;
    proto.instr_count  = module.instr_count;
    proto.constants    = module.constants;
    proto.const_count  = module.const_count;
    proto.ic_count     = module.ic_count;
    proto.ic_names     = module.ic_names;

    UClosure cl;
    memset(&cl, 0, sizeof(cl));
    cl.proto    = &proto;
    cl.nupvals  = 0;

    UValue out    = {0};
    int    threw  = 0;
    int    rc     = urbi_run_closure_on_scratch(&vm, &cl, &out, &threw);

    UASSERT_EQ(0, rc);
    UASSERT_EQ(0, threw);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(42, (int)out.v.i);

    umodule_destroy(&module);
    uarena_destroy(&arena);
    uvm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_uwatcher_scratch_suite(void)
{
    printf("test_uwatcher_scratch\n");
    utest_run("scratch_runner_returns_integer_value",
              scratch_runner_returns_integer_value);
}
