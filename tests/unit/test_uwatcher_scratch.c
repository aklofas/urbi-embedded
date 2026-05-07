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
#include "runtime/uclosure.h"
#include "value/uarena.h"
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

/* scratch_runner_sets_threw_on_unhandled_throw
 *
 * Compile "nil()" — calling a non-closure value triggers OP_CALL's
 * "callee is not a closure" type-error path (vm_format_type_error_msg,
 * which is safe with a NULL strand.module).  The dispatch loop halts
 * with vm->last_error == UVM_TYPE_ERROR.  The helper must:
 *   - set *out_threw = 1
 *   - reset vm->last_error to UVM_OK
 *   - leave *out_result as UVAL_NIL */
UTEST(scratch_runner_sets_threw_on_unhandled_throw)
{
    UVM    vm;
    UArena arena;
    UModule module;

    uvm_init(&vm, NULL, NULL);
    uarena_init(&arena, 4096);
    memset(&module, 0, sizeof(module));

    int ok = compile_source(&vm, &arena, &module, "nil()");
    UASSERT(ok);

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
    cl.proto   = &proto;
    cl.nupvals = 0;

    UValue out   = {0};
    int    threw = 0;
    int    rc    = urbi_run_closure_on_scratch(&vm, &cl, &out, &threw);

    UASSERT_EQ(0, rc);
    UASSERT_EQ(1, threw);
    UASSERT_EQ((int)UVAL_NIL, (int)out.kind);

    /* Helper must reset vm->last_error so the caller's next VM operation
     * does not see the cond's stale error state. */
    UASSERT_EQ((int)UVM_OK, (int)vm.last_error);

    umodule_destroy(&module);
    uarena_destroy(&arena);
    uvm_destroy(&vm);
}

/* scratch_runner_handles_null_closure
 *
 * Passing NULL as the closure must return 0 immediately with
 * *out_result = UVAL_NIL and *out_threw = 0.  This models the
 * watcher-installed-without-condition contract. */
UTEST(scratch_runner_handles_null_closure)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* Pre-poison the out params to confirm the helper overwrites them. */
    UValue out;
    out.kind  = (uint8_t)UVAL_INT;
    out.v.i   = 999;
    int threw = 1;

    int rc = urbi_run_closure_on_scratch(&vm, NULL, &out, &threw);

    UASSERT_EQ(0, rc);
    UASSERT_EQ(0, threw);
    UASSERT_EQ((int)UVAL_NIL, (int)out.kind);

    uvm_destroy(&vm);
}

/* scratch_runner_returns_nil_for_nil_literal
 *
 * Compile "nil", run via urbi_run_closure_on_scratch, and verify
 * out.kind == UVAL_NIL with threw == 0.  Mirrors T3's integer test. */
UTEST(scratch_runner_returns_nil_for_nil_literal)
{
    UVM    vm;
    UArena arena;
    UModule module;

    uvm_init(&vm, NULL, NULL);
    uarena_init(&arena, 4096);
    memset(&module, 0, sizeof(module));

    int ok = compile_source(&vm, &arena, &module, "nil");
    UASSERT(ok);

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

    UValue out   = {0};
    int    threw = 0;
    int    rc    = urbi_run_closure_on_scratch(&vm, &cl, &out, &threw);

    UASSERT_EQ(0, rc);
    UASSERT_EQ(0, threw);
    UASSERT_EQ((int)UVAL_NIL, (int)out.kind);

    umodule_destroy(&module);
    uarena_destroy(&arena);
    uvm_destroy(&vm);
}

/* scratch_runner_returns_true_for_truthy_comparison
 *
 * Compile "5 > 3", run via urbi_run_closure_on_scratch, and verify
 * out.kind == UVAL_BOOL with out.v.i == 1.  Models the typical at(cond)
 * shape where cond is a comparison expression that evaluates to true. */
UTEST(scratch_runner_returns_true_for_truthy_comparison)
{
    UVM    vm;
    UArena arena;
    UModule module;

    uvm_init(&vm, NULL, NULL);
    uarena_init(&arena, 4096);
    memset(&module, 0, sizeof(module));

    int ok = compile_source(&vm, &arena, &module, "5 > 3");
    UASSERT(ok);

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
    cl.proto   = &proto;
    cl.nupvals = 0;

    UValue out   = {0};
    int    threw = 0;
    int    rc    = urbi_run_closure_on_scratch(&vm, &cl, &out, &threw);

    UASSERT_EQ(0, rc);
    UASSERT_EQ(0, threw);
    UASSERT_EQ((int)UVAL_BOOL, (int)out.kind);
    UASSERT_EQ(1, (int)out.v.i);

    umodule_destroy(&module);
    uarena_destroy(&arena);
    uvm_destroy(&vm);
}

/* scratch_runner_returns_false_for_falsy_comparison
 *
 * Compile "1 > 3", run via urbi_run_closure_on_scratch, and verify
 * out.kind == UVAL_BOOL with out.v.i == 0.  Models the typical at(cond)
 * shape where cond is a comparison expression that evaluates to false. */
UTEST(scratch_runner_returns_false_for_falsy_comparison)
{
    UVM    vm;
    UArena arena;
    UModule module;

    uvm_init(&vm, NULL, NULL);
    uarena_init(&arena, 4096);
    memset(&module, 0, sizeof(module));

    int ok = compile_source(&vm, &arena, &module, "1 > 3");
    UASSERT(ok);

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
    cl.proto   = &proto;
    cl.nupvals = 0;

    UValue out   = {0};
    int    threw = 0;
    int    rc    = urbi_run_closure_on_scratch(&vm, &cl, &out, &threw);

    UASSERT_EQ(0, rc);
    UASSERT_EQ(0, threw);
    UASSERT_EQ((int)UVAL_BOOL, (int)out.kind);
    UASSERT_EQ(0, (int)out.v.i);

    umodule_destroy(&module);
    uarena_destroy(&arena);
    uvm_destroy(&vm);
}

/* scratch_runner_with_payload_writes_r0
 *
 * Hand-rolled proto with a single OP_RET R0, 0, 0 instruction.  After
 * urbi_strand_arm_from_closure zeroes the register stack, the payload
 * variant must write the supplied UValue into R[0]; OP_RET R0 then
 * returns it via out_slot.
 *
 * The hand-rolled approach (rather than urbiscript source compiled
 * through the emitter) is the only reliable way to test that the payload
 * lands at R[0] specifically — the emitter is free to allocate temporary
 * registers however it likes for source like "x" or "42". */
UTEST(scratch_runner_with_payload_writes_r0)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* One-instruction proto: OP_RET R0, 0, 0. */
    uint32_t instrs[1];
    instrs[0] = uinstr_enc_abc(OP_RET, 0, 0, 0);

    UProto proto;
    memset(&proto, 0, sizeof(proto));
    proto.instructions = instrs;
    proto.instr_count  = 1;

    UClosure cl;
    memset(&cl, 0, sizeof(cl));
    cl.proto   = &proto;
    cl.nupvals = 0;

    UValue payload = {0};
    payload.kind = (uint8_t)UVAL_INT;
    payload.v.i  = 1234;

    UValue out   = {0};
    int    threw = 0;
    int    rc    = urbi_run_closure_on_scratch_with_payload(
                       &vm, &cl, payload, &out, &threw);

    UASSERT_EQ(0, rc);
    UASSERT_EQ(0, threw);
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(1234, (int)out.v.i);

    uvm_destroy(&vm);
}

/* scratch_runner_with_payload_handles_null_closure
 *
 * The payload-variant must mirror the no-payload variant's NULL-closure
 * contract: return 0, *out_result = nil, *out_threw = 0.  The supplied
 * payload is harmlessly ignored (nothing dereferences it before the
 * NULL-closure early-return). */
UTEST(scratch_runner_with_payload_handles_null_closure)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* Pre-poison the out params to confirm the helper overwrites them. */
    UValue out;
    out.kind  = (uint8_t)UVAL_INT;
    out.v.i   = 999;
    int threw = 1;

    UValue payload = {0};
    payload.kind = (uint8_t)UVAL_INT;
    payload.v.i  = 7;

    int rc = urbi_run_closure_on_scratch_with_payload(
                 &vm, NULL, payload, &out, &threw);

    UASSERT_EQ(0, rc);
    UASSERT_EQ(0, threw);
    UASSERT_EQ((int)UVAL_NIL, (int)out.kind);

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
    utest_run("scratch_runner_sets_threw_on_unhandled_throw",
              scratch_runner_sets_threw_on_unhandled_throw);
    utest_run("scratch runner handles NULL closure",
              scratch_runner_handles_null_closure);
    utest_run("scratch runner returns nil for nil literal",
              scratch_runner_returns_nil_for_nil_literal);
    utest_run("scratch_runner_returns_true_for_truthy_comparison",
              scratch_runner_returns_true_for_truthy_comparison);
    utest_run("scratch_runner_returns_false_for_falsy_comparison",
              scratch_runner_returns_false_for_falsy_comparison);
    utest_run("scratch_runner_with_payload_writes_r0",
              scratch_runner_with_payload_writes_r0);
    utest_run("scratch_runner_with_payload_handles_null_closure",
              scratch_runner_with_payload_handles_null_closure);
}
