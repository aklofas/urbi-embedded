/* SPDX-License-Identifier: BSD-3-Clause */
/* Runtime invariants F2: OP_CLOSURE dispatch guards fire in release builds.
 *
 * Three URBI_DISPATCH_ASSERT calls at the OP_CLOSURE site were promoted to
 * unconditional HALT-with-TYPE_ERROR paths (runtime-invariants finding F2).
 * These tests verify each promoted guard by constructing a UProto tree
 * directly (bypassing deserialization — the W7 verifier would reject
 * malformed bytecode at load time, so the runtime guards defend
 * programmatically-constructed trees and any future API path that builds
 * UProto without the serializer).
 *
 * Because urbi_vm_dispatch_loop_until_yield is an internal API, tests include
 * uvm.h and sched/ustrand.h — mirrors the approach in test_dispatch_loop.c.
 *
 * Refcount discipline (critical):
 *   - call urbi_proto_strand_ref_acquire before dispatch (strand-bind)
 *   - dispatch internally calls urbi_vm_alloc_closure → closure-bind +1
 *   - ustrand_destroy → strand-bind -1
 *   - urbi_vm_destroy GC sweep → uclosure_destroy → closure-bind -1
 *   - Net: refcount reaches 0, no underflow URBI_REQUIRE fires.
 *   - Destroy VM BEFORE freeing the proto tree (GC finalizers need live ptrs). */

#include "utest.h"

#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "chunk/uchunk.h"
#include "object/uchunk_instance.h"
#include "runtime/uclosure.h"
#include "sched/usched_cooperative.h"

#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* -----------------------------------------------------------------------
 * Bytecode helpers
 * ----------------------------------------------------------------------- */

/* OP_CLOSURE R[A] Bx=nested_index; ABx encoding. */
static uint32_t enc_closure(uint8_t a, uint16_t bx)
{
    return uinstr_enc_abx(OP_CLOSURE, a, bx);
}

static uint32_t enc_ret(uint8_t a)
{
    return uinstr_enc_abc(OP_RET, a, 0, 0);
}

/* -----------------------------------------------------------------------
 * Strand setup (mirrors test_dispatch_loop.c)
 * ----------------------------------------------------------------------- */

static void ocl_strand_setup(UStrand *s, UVM *vm,
                             const uint32_t *instructions,
                             const UValue   *constants,
                             UValue         *reg_stack,
                             UProto         *root_proto,
                             UChunkInstance *module_instance)
{
    memset(s, 0, sizeof(*s));
    s->vm              = vm;
    s->state           = USTRAND_STATE_RUNNING;
    s->stack           = reg_stack;
    s->R               = reg_stack;
    s->pc              = instructions;
    s->pc_base         = instructions;
    s->cur_consts      = constants;
    s->frame_count     = 0;
    s->open_upvals     = NULL;
    s->out_slot        = NULL;
    s->root_proto      = root_proto;
    s->module_instance = module_instance;
    /* Strand-bind refcount bump: paired with the uproto_strand_refcount_dec
     * inside ustrand_destroy (via uproto_strand_ref_release).  Must precede
     * urbi_vm_dispatch_loop_until_yield so the dec on teardown does not underflow. */
    if (root_proto != NULL)
        urbi_proto_strand_ref_acquire(root_proto, URBI_PROTO_REF_OWNER_TRANSIENT);
}

/* -----------------------------------------------------------------------
 * Proto tree builders
 *
 * Build a root UProto with one nested child.  The root's instruction stream
 * is: OP_CLOSURE R[0] Bx=0 (bind nested[0]), OP_RET R[0].
 * The child has no instructions beyond OP_RET.
 *
 * Both UProto structs are embedded inside OCLProtoTree — the structs
 * themselves are not heap-allocated.  Only root.nested[] is heap-allocated
 * so that uchunk_destroy's normal nested-array free is not needed (the test
 * calls ocl_free_proto_tree explicitly).  heap_allocated=false tells
 * uclosure_destroy not to try to free the struct. */

#define MAX_INSTRS 4

typedef struct {
    UProto   root;
    UProto   child;
    uint32_t root_instrs[MAX_INSTRS];
    int8_t   root_deltas[MAX_INSTRS];
    uint32_t child_instrs[1];
    int8_t   child_deltas[1];
} OCLProtoTree;

static void ocl_build_proto_tree(OCLProtoTree *t)
{
    memset(t, 0, sizeof(*t));

    /* Child proto: minimal (OP_RET R[0]).  heap_allocated=false (embedded). */
    t->child_instrs[0] = enc_ret(0);
    t->child_deltas[0] = 1;
    t->child.max_reg    = 0;
    t->child.nupvals    = 0;
    t->child.nparams    = 0;
    t->child.instructions = t->child_instrs;
    t->child.instr_count  = 1;
    t->child.instr_cap    = 1;
    t->child.line_deltas  = t->child_deltas;
    t->child.ic_index     = 1;   /* DFS pre-order: root=0, child=1 */
    t->child.root         = &t->root;  /* back-pointer so uproto_root_of works */
    t->child.heap_allocated = false;

    /* Root proto: OP_CLOSURE R[0] Bx=0 then OP_RET R[0].
     * nested[0] = &child; nested_count = 1.
     * heap_allocated=false — we manage the struct lifetime explicitly. */
    t->root_instrs[0] = enc_closure(0, 0);  /* Bx=0 → nested[0] */
    t->root_instrs[1] = enc_ret(0);
    t->root_deltas[0] = 1;
    t->root_deltas[1] = 0;
    t->root.max_reg      = 0;
    t->root.nupvals      = 0;
    t->root.nparams      = 0;
    t->root.instructions = t->root_instrs;
    t->root.instr_count  = 2;
    t->root.instr_cap    = MAX_INSTRS;
    t->root.line_deltas  = t->root_deltas;
    t->root.ic_index     = 0;
    t->root.heap_allocated = false;
    t->root.total_proto_count = 2;
    t->root.next_proto_serial = 2;

    /* Wire nested array — heap-allocated so the pointer is stable. */
    t->root.nested = (UProto **)malloc(sizeof(UProto *));
    t->root.nested[0]    = &t->child;
    t->root.nested_count = 1;
    t->root.nested_cap   = 1;
}

/* Free only the heap-allocated parts inside OCLProtoTree.
 * Must be called AFTER urbi_vm_destroy (GC finalizers need valid proto ptrs). */
static void ocl_free_proto_tree(OCLProtoTree *t)
{
    free(t->root.nested);
    t->root.nested = NULL;
}

/* -----------------------------------------------------------------------
 * Common teardown: destroy strand (drops strand-bind ref, frees reg_stack via
 * urbi_strand_register_stack_free), then destroy VM (GC sweep finalizes
 * UClosure → closure-bind dec), then free the proto tree.
 * ----------------------------------------------------------------------- */

static void ocl_teardown(UStrand *s, UVM *vm, OCLProtoTree *t)
{
    /* ustrand_destroy drops strand-bind ref AND frees s->stack via
     * urbi_strand_register_stack_free — do NOT free reg_stack separately. */
    ustrand_destroy(s, vm);
    /* Destroy VM second: GC sweep finalizes UClosure cells (closure-bind dec). */
    urbi_vm_destroy(vm);
    ocl_free_proto_tree(t);
}

/* -----------------------------------------------------------------------
 * Test 1: OP_CLOSURE with owning_module_instance == NULL → TYPE_ERROR
 * ----------------------------------------------------------------------- */

UTEST(op_closure_null_owning_mi_raises_type_error)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    OCLProtoTree t;
    ocl_build_proto_tree(&t);

    /* Leave child.owning_module_instance == NULL (zero-initialized above).
     * This simulates a proto tree built without calling
     * urbi_chunk_instance_create — the "not yet instantiated" state the
     * guard is designed to catch. */
    UASSERT(t.child.owning_module_instance == NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    UStrand s;
    /* module_instance on the strand is NULL — the guard fires on
     * child_proto->owning_module_instance, not on the strand field. */
    ocl_strand_setup(&s, &vm, t.root_instrs, NULL, reg_stack,
                     &t.root, /*module_instance*/ NULL);

    vm.last_error = UVM_OK;
    (void)urbi_vm_dispatch_loop_until_yield(&s, /* step_budget */ 10000ULL);

    UASSERT_EQ((int)UVM_TYPE_ERROR, (int)vm.last_error);
    /* Error message must mention CLOSURE. */
    UASSERT(strstr(vm.last_errmsg, "CLOSURE") != NULL);

    ocl_teardown(&s, &vm, &t);
    /* reg_stack freed by ustrand_destroy → urbi_strand_register_stack_free. */
}

/* -----------------------------------------------------------------------
 * Test 2: OP_CLOSURE with proto_instances == NULL → TYPE_ERROR
 * ----------------------------------------------------------------------- */

UTEST(op_closure_null_proto_instances_raises_type_error)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    OCLProtoTree t;
    ocl_build_proto_tree(&t);

    /* Wire a non-NULL owning_module_instance whose proto_instances is NULL.
     * Use a stack-local UChunkInstance — no GC involvement needed since the
     * test halts before cl->proto_inst is written. */
    struct UChunkInstance fake_mi;
    memset(&fake_mi, 0, sizeof(fake_mi));
    /* fake_mi.proto_instances remains NULL — that is the invariant under test. */
    t.child.owning_module_instance = &fake_mi;

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    UStrand s;
    ocl_strand_setup(&s, &vm, t.root_instrs, NULL, reg_stack,
                     &t.root, /*module_instance*/ NULL);

    vm.last_error = UVM_OK;
    (void)urbi_vm_dispatch_loop_until_yield(&s, /* step_budget */ 10000ULL);

    UASSERT_EQ((int)UVM_TYPE_ERROR, (int)vm.last_error);
    UASSERT(strstr(vm.last_errmsg, "CLOSURE") != NULL);

    ocl_teardown(&s, &vm, &t);
}

/* -----------------------------------------------------------------------
 * Test 3: OP_CLOSURE with ic_index >= proto_instances->n → TYPE_ERROR
 * ----------------------------------------------------------------------- */

UTEST(op_closure_ic_index_out_of_range_raises_type_error)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    OCLProtoTree t;
    ocl_build_proto_tree(&t);

    /* Build a minimal UProtoInstanceArr with n=1 and wire it to a
     * fake UChunkInstance.  Set ic_index=99 (well beyond n=1). */
    struct UChunkInstance fake_mi;
    memset(&fake_mi, 0, sizeof(fake_mi));

    /* Allocate a UProtoInstanceArr with room for 1 entry.  The guard fires
     * BEFORE accessing entries[], so the content doesn't matter. */
    UProtoInstanceArr *arr = (UProtoInstanceArr *)calloc(
        1, sizeof(UProtoInstanceArr) + sizeof(UProtoInstance));
    UASSERT(arr != NULL);
    arr->n = 1;
    fake_mi.proto_instances = arr;

    /* Force ic_index out of bounds. */
    t.child.owning_module_instance = &fake_mi;
    t.child.ic_index = 99;   /* >= arr->n (which is 1) */

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    UStrand s;
    ocl_strand_setup(&s, &vm, t.root_instrs, NULL, reg_stack,
                     &t.root, /*module_instance*/ NULL);

    vm.last_error = UVM_OK;
    (void)urbi_vm_dispatch_loop_until_yield(&s, /* step_budget */ 10000ULL);

    UASSERT_EQ((int)UVM_TYPE_ERROR, (int)vm.last_error);
    UASSERT(strstr(vm.last_errmsg, "CLOSURE") != NULL);

    free(arr);
    ocl_teardown(&s, &vm, &t);
    /* reg_stack freed by ustrand_destroy → urbi_strand_register_stack_free. */
}

/* -----------------------------------------------------------------------
 * Test 4: OP_CLOSURE happy path — valid owning_module_instance → UVM_OK
 *
 * Verifies the promoted guards do not reject a correctly-wired proto tree.
 * Uses the full emit pipeline so urbi_chunk_instance_create populates
 * owning_module_instance.  Exercises the same OP_CLOSURE dispatch path
 * as the three error tests.
 * ----------------------------------------------------------------------- */

#include "lex/ulex.h"
#include "parse/uparse.h"
#include "emit/uemit.h"
#include "value/uarena.h"
#include "parse/uast.h"

UTEST(op_closure_valid_owning_mi_succeeds)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UArena arena;
    uarena_init(&arena, 4096);

    UProto mod;
    memset(&mod, 0, sizeof(mod));

    /* Compile a function literal: OP_CLOSURE fires when f() is called. */
    const char *src = "var f = function() { 42 }; f()";
    ULexer   lex;
    UParser  p;
    UEmitter e;
    UAstNode *node;

    ulex_init(&lex, src, strlen(src));
    uemit_init(&e, &mod, &arena, &vm, NULL);
    uparse_init(&p, &lex, &arena);

    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) break;
        (void)uemit_statement(&e, node);
        uarena_reset(&arena);
    }
    int emit_ok = (uemit_finish(&e) == EMIT_OK);
    UASSERT(emit_ok);
    UASSERT(mod.nested_count >= 1);

    UValue out;
    memset(&out, 0, sizeof(out));
    UVMError rc = urbi_vm_run(&vm, NULL, &mod, &out);
    UASSERT_EQ((int)UVM_OK, (int)rc);
    /* f() returns 42 (last expression). */
    UASSERT_EQ((int)UVAL_INT, (int)out.kind);
    UASSERT_EQ(42, (int)out.v.i);

    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
    uchunk_destroy(&mod, NULL);
}

/* -----------------------------------------------------------------------
 * Suite entry
 * ----------------------------------------------------------------------- */

void
test_op_closure_invariants_suite(void)
{
    utest_run("op_closure_invariants: null owning_mi raises TYPE_ERROR",
              op_closure_null_owning_mi_raises_type_error);
    utest_run("op_closure_invariants: null proto_instances raises TYPE_ERROR",
              op_closure_null_proto_instances_raises_type_error);
    utest_run("op_closure_invariants: ic_index out-of-range raises TYPE_ERROR",
              op_closure_ic_index_out_of_range_raises_type_error);
    utest_run("op_closure_invariants: valid owning_mi succeeds (regression guard)",
              op_closure_valid_owning_mi_succeeds);
}
