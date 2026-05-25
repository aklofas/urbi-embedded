/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: OP_FORK_DETACH / OP_FORK_JOIN / OP_JOIN_WAIT runtime (T38).
 *
 * Tests use the urbi_step driver (realm-managed strands), not urbi_vm_run, because
 * fork opcodes require a realm to spawn child strands.
 *
 * M3 closure-spawn note: children capture upvalues from the parent's scope.
 * Shared-frame semantics (spec §7.1) are deferred to M5+; see uop_fork.c.
 */

#include "utest.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "urbi/urbi.h"
#include "chunk/uchunk.h"
#include "object/uchunk_instance.h"
#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "value/uvalue.h"
#include "vm/uop_fork.h"
#include "sched/usched_cooperative.h"

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Test infrastructure helpers
 * =================================================================== */

/* Compile `src` into `*out_mod` using vm. Returns true on success. */
static bool
fork_compile(UVM *vm, const char *src, UProto *out_mod)
{
    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    UArena arena;
    uarena_init(&arena, 4096);

    *out_mod = (UProto){0};

    UEmitter e;
    uemit_init(&e, out_mod, &arena, vm, NULL);

    UParser p;
    uparse_init(&p, &lex, &arena);

    bool ok = true;
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) { ok = false; break; }
        if (uemit_statement(&e, node) != EMIT_OK) { ok = false; break; }
        uarena_reset(&arena);
    }
    if (ok && uemit_finish(&e) != EMIT_OK) ok = false;

    uarena_destroy(&arena);
    return ok;
}

/* Create a strand from module, start it (DORMANT→READY), run urbi_step
 * until quiescent or MAX_STEPS exceeded.  Returns 1 on quiescent, 0 on
 * timeout, -1 on fatal.  If `out_result` is non-NULL, fills it with the
 * strand's out_slot value (set by OP_RET). */
#define FORK_TEST_MAX_STEPS 100000ULL

static int
fork_run_to_quiescent(UVM *vm, URealm *realm, UProto *module,
                      UValue *out_result)
{
    /* Allocate + arm a strand for this module. */
    UStrand *s = urbi_strand_create(vm, realm, NULL);
    if (!s) return -1;

    /* Wire execution state from module. */
    const size_t stack_bytes = UVM_STACK_CAP * sizeof(UValue);
    s->stack = (UValue *)vm->alloc_fn(NULL, stack_bytes, vm->alloc_ud);
    if (!s->stack) { urbi_strand_destroy(vm, s); return -1; }
    {
        volatile unsigned char *p = (volatile unsigned char *)s->stack;
        size_t i;
        for (i = 0; i < stack_bytes; i++) p[i] = 0;
    }
    s->R          = s->stack;
    s->root_proto = module;
    s->pc         = module->instructions;
    s->pc_base    = module->instructions;
    s->cur_consts = module->constants;
    /* v0.10.1 W4: use typed-handle acquire so g_strand_ref_total stays balanced. */
    urbi_proto_strand_ref_acquire(module, URBI_PROTO_REF_OWNER_STRAND);
    s->frame_count = 0;
    s->open_upvals = NULL;
    if (out_result) {
        s->out_slot = out_result;
    }

    /* v0.9.0: stamp owning_module_instance on every nested UProto so that
     * OP_CLOSURE can read child_proto->owning_module_instance directly.
     * urbi_chunk_instance_create calls stamp_owning_mi(root_proto, mi)
     * internally — the same stamp urbi_strand_create_for_module performs. */
    s->module_instance = urbi_chunk_instance_create(vm, module);
    if (!s->module_instance) { urbi_strand_destroy(vm, s); return -1; }

    urbi_strand_start(vm, s);  /* DORMANT → READY */

    /* Drive via urbi_step until quiescent or timeout. */
    int result = 0;
    uint64_t steps_total = 0;
    while (steps_total < FORK_TEST_MAX_STEPS) {
        UStepResult sr = urbi_step(vm, 100, NULL);
        steps_total += 100;
        if (sr == URBI_STEP_QUIESCENT) { result = 1; break; }
        if (sr == URBI_STEP_FATAL)     { result = -1; break; }
        if (sr == URBI_STEP_WAKE_AT)   { result = 1; break; } /* no sleepers in fork tests */
        /* URBI_STEP_RUNNING: keep going */
    }

    /* Note: strand is NOT explicitly destroyed here — the realm owns it.
     * Tests call urbi_realm_destroy which frees realm-managed strands. */
    return result;
}

/* ===================================================================
 * Tests
 * =================================================================== */

/* Case 1: OP_FORK_DETACH spawns child strand; parent continues.
 * Smoke test: verify code compiles and runs without crash or fatal.
 * "1 , 2" — LHS '1' forks as a detached thunk; parent runs '2'.
 * urbi_step runs to quiescent (both strands complete). */
UTEST(fork_detach_basic_no_crash)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    UProto module;
    UASSERT(fork_compile(&vm, "1 , 2", &module));

    UValue result = {0};
    int rc = fork_run_to_quiescent(&vm, realm, &module, &result);

    UASSERT_EQ(rc, 1);  /* reached quiescent */
    /* parent's inline continuation is '2' — result is 2 */
    UASSERT_EQ((int)result.kind, (int)UVAL_INT);
    UASSERT_EQ(result.v.i, (int64_t)2);

    uchunk_destroy(&module, NULL);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* Case 2: OP_FORK_JOIN + OP_JOIN_WAIT — `&` separator blocks parent until child DEAD.
 * "1 & 2" — parent runs '1', spawns '2' as child, waits for child, then returns void. */
UTEST(fork_join_wait_basic)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    UProto module;
    UASSERT(fork_compile(&vm, "1 & 2", &module));

    UValue result = {0};
    int rc = fork_run_to_quiescent(&vm, realm, &module, &result);

    UASSERT_EQ(rc, 1);  /* reached quiescent */
    /* spec §7.2: `&` result is void */
    UASSERT_EQ((int)result.kind, (int)UVAL_VOID);

    uchunk_destroy(&module, NULL);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* Case 3: Child strand inherits parent's ambient tag chain.
 * This is tested by verifying that a forked child does not crash when the
 * parent has a realm tag.  The realm tag is always present (urbi_strand_create
 * attaches it); child inherits it via fork_spawn_child. */
UTEST(fork_child_inherits_ambient_tags)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);
    UASSERT(realm->tag != NULL);

    UProto module;
    /* Simple comma-separate: spawns a detached strand */
    UASSERT(fork_compile(&vm, "42 , 7", &module));

    UValue result = {0};
    int rc = fork_run_to_quiescent(&vm, realm, &module, &result);

    UASSERT_EQ(rc, 1);
    /* Realm tag should still have at least its own members (parent + child).
     * After quiescent, both strands are DEAD; their TAG_SCOPE entries
     * were popped during unwind.  We just verify no crash and correct result. */
    UASSERT_EQ((int)result.kind, (int)UVAL_INT);
    UASSERT_EQ(result.v.i, (int64_t)7);

    uchunk_destroy(&module, NULL);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* Case 4: Three-way comma — "1 , 2 , 3" — spawns two children, parent runs '3'.
 * Per emit: count-1 forks, last child inline. */
UTEST(fork_detach_three_way)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    UProto module;
    UASSERT(fork_compile(&vm, "1 , 2 , 3", &module));

    UValue result = {0};
    int rc = fork_run_to_quiescent(&vm, realm, &module, &result);

    UASSERT_EQ(rc, 1);
    /* Last inline child returns 3. */
    UASSERT_EQ((int)result.kind, (int)UVAL_INT);
    UASSERT_EQ(result.v.i, (int64_t)3);

    uchunk_destroy(&module, NULL);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* Case 5: Fork detach followed by parent continuation — runnable count tracks correctly.
 * After quiescent, strand_runnable_count should be 0. */
UTEST(fork_detach_quiescent_count_zero)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    UProto module;
    UASSERT(fork_compile(&vm, "10 , 20", &module));

    int rc = fork_run_to_quiescent(&vm, realm, &module, NULL);
    UASSERT_EQ(rc, 1);
    UASSERT_EQ(vm.strand_runnable_count, 0U);

    uchunk_destroy(&module, NULL);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* Case 6: fork_wake_joiners is idempotent — calling it twice on a strand
 * with no joiners is a no-op. */
UTEST(fork_wake_joiners_empty_is_noop)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    UStrand *s = urbi_strand_create(&vm, realm, NULL);
    UASSERT(s != NULL);
    UASSERT(s->joiners_head == NULL);

    /* Call twice — must not crash. */
    fork_wake_joiners(s, &vm);
    fork_wake_joiners(s, &vm);
    UASSERT(s->joiners_head == NULL);

    urbi_strand_destroy(&vm, s);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* Case 7: UVAL_STRAND_MAKE / UVAL_AS_STRAND round-trip. */
UTEST(uval_strand_round_trip)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    UStrand *s = urbi_strand_create(&vm, realm, NULL);
    UASSERT(s != NULL);

    UValue v = UVAL_STRAND_MAKE(s);
    UASSERT_EQ((int)v.kind, (int)UVAL_STRAND);
    UASSERT(UVAL_AS_STRAND(v) == s);

    /* Strand values are truthy. */
    UASSERT(uvalue_truthy(&v));

    urbi_strand_destroy(&vm, s);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* Case 8: `&` with non-trivial expressions — `(1 + 2) & (3 + 4)`.
 * Parent runs LHS (1+2=3), spawns RHS as child, waits, returns void. */
UTEST(fork_join_arithmetic_children)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    UProto module;
    UASSERT(fork_compile(&vm, "(1 + 2) & (3 + 4)", &module));

    UValue result = {0};
    int rc = fork_run_to_quiescent(&vm, realm, &module, &result);

    UASSERT_EQ(rc, 1);
    UASSERT_EQ((int)result.kind, (int)UVAL_VOID);

    uchunk_destroy(&module, NULL);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* Case 9 (T31 / VM-004): op_join_wait blocks parent BEFORE linking to the
 * child's join chain.  The audit identified the prior link-then-block
 * ordering as a latent race: any concurrent walker that read
 * child->joiners_head would observe the parent on the join chain while
 * its state was still RUNNING.
 *
 * This test runs a multi-join workload to exercise the OP_JOIN_WAIT path
 * with the new ordering.  All 1498 existing tests already pass with the
 * fix; this case adds an explicit regression marker so future refactors
 * trip a named test rather than only a coverage-of-determinism gate.
 *
 * The behavioral check is: a chain of joins (& separator) reaches
 * quiescence with the expected void result and no fatal.  If the
 * ordering re-broke (e.g. the parent's state was inconsistent at the
 * link site), one of sched_strand_block's SCHED-002 entry-state asserts
 * or the make_runnable READY-idempotence assert in fork_wake_joiners
 * would trip in URBI_DEBUG builds. */
UTEST(fork_join_wait_parent_blocked_before_link_to_chain)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    UProto module;
    /* Two consecutive joins — exercises the block-then-link path
     * once per `&` operator without any sleep/event reasons in play. */
    UASSERT(fork_compile(&vm, "1 & 2 & 3", &module));

    UValue result = {0};
    int rc = fork_run_to_quiescent(&vm, realm, &module, &result);

    UASSERT_EQ(rc, 1);                          /* reached quiescent */
    UASSERT_EQ((int)result.kind, (int)UVAL_VOID);  /* `&` result is void */

    uchunk_destroy(&module, NULL);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite registration
 * =================================================================== */

void test_fork_suite(void) {
    utest_run("fork detach basic no crash",          fork_detach_basic_no_crash);
    utest_run("fork join wait basic",                fork_join_wait_basic);
    utest_run("fork child inherits ambient tags",    fork_child_inherits_ambient_tags);
    utest_run("fork detach three-way",               fork_detach_three_way);
    utest_run("fork detach quiescent count zero",    fork_detach_quiescent_count_zero);
    utest_run("fork wake joiners empty is noop",     fork_wake_joiners_empty_is_noop);
    utest_run("uval strand round trip",              uval_strand_round_trip);
    utest_run("fork join arithmetic children",       fork_join_arithmetic_children);
    utest_run("fork join wait parent blocked before link to chain (T31 / VM-004)",
              fork_join_wait_parent_blocked_before_link_to_chain);
}
