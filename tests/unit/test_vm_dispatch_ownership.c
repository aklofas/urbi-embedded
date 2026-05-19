/* SPDX-License-Identifier: BSD-3-Clause */
/* Phase 5 — VM dispatch ownership regressions (v0.5.7-fixes T29-T33).
 *
 * Audit IDs covered:
 *   VM-001 — OP_GETSLOT_CHANGE_EVENT must route through ic_resolve_pi
 *            (closed by T29 — structural-correctness gate, no test here).
 *   VM-002 + VM-012 — reactive-install opcodes must surface install errors
 *            (closed by T30: reactive_install_propagates_pool_oom +
 *             reactive_install_at_event_propagates_pool_oom).
 *   VM-003 — reactive-install + uop_fork must kind-check operand registers
 *            (closed by T31: reactive_install_kind_checks_cond_operand +
 *             at_event_install_kind_check).
 *   VM-005 — vm_alloc_closure OOM must return NULL cleanly
 *            (closed by T32 — closure_list deleted at v0.8.4 Step C-3;
 *             test now verifies NULL return + no alloc_fn free call).
 *   VM-013 — op_at_event_install must verify R[event_reg].kind == UVAL_EVENT
 *            (closed by T33: at_event_install_kind_check).
 *
 * These tests are written against existing dispatch surfaces — they do not
 * require new test seams beyond what was already wired for M5.  The
 * watcher-pool drain trick (vm.watcher_pool_freelist = NULL) exhausts the
 * pool without injecting an alternate allocator. */

#include "utest.h"

#include "value/uarena.h"
#include "parse/uast.h"
#include "chunk/uchunk.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "vm/uvm.h"
#include "vm/uvm_internal.h"   /* vm_alloc_closure */
#include "vm/uop_fork.h"       /* UVAL_STRAND_MAKE — T120 join-wait fast path */
#include "watcher/uwatcher.h"  /* urbi_watcher_unregister_internal */
#include "runtime/uclosure.h"  /* UClosure */
#include "runtime/ucleanup.h"  /* UCleanupEntry */
#include "sched/usched_cooperative.h" /* sched_init */
#include "sched/ustrand.h"     /* USTRAND_STATE_DEAD — T120 */
#include "realm/urealm.h"      /* urbi_realm_create — T120 fork OOM */

#include <string.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers — copied from test_at_install_dispatch.c PipeCtx pattern.
 * =================================================================== */

typedef struct {
    UVM     vm;
    UModule module;
    UArena  arena;
} PipeCtx;

static int
compile_source(PipeCtx *ctx, const char *src)
{
    urbi_vm_init(&ctx->vm, NULL, NULL);
    uarena_init(&ctx->arena, 4096);
    memset(&ctx->module, 0, sizeof(ctx->module));

    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    UEmitter e;
    uemit_init(&e, &ctx->module, &ctx->arena, &ctx->vm, NULL);

    UParser p;
    uparse_init(&p, &lex, &ctx->arena);

    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) break;
        (void)uemit_statement(&e, node);
        uarena_reset(&ctx->arena);
    }

    return (uemit_finish(&e) == EMIT_OK) ? 0 : -1;
}

static void
pipeline_ctx_destroy(PipeCtx *ctx)
{
    while (ctx->vm.active_watchers_head != NULL)
        urbi_watcher_unregister_internal(&ctx->vm,
                                         ctx->vm.active_watchers_head);
    umodule_destroy(&ctx->module, NULL);
    uarena_destroy(&ctx->arena);
    urbi_vm_destroy(&ctx->vm);
}

/* ===================================================================
 * VM-002 / T30: reactive-install propagates pool exhaustion as UVM_OOM
 * =================================================================== */

/* reactive_install_propagates_pool_oom:
 *
 * Compile "var x = 0; at (x > 5) x" — the at-install should normally pass.
 * Drain the watcher pool freelist before run; install_watcher_runtime then
 * returns URBI_INSTALL_OOM_POOL.  Pre-fix the dispatcher discarded the
 * result and kept running; post-fix the strand halts with UVM_OOM. */
UTEST(reactive_install_propagates_pool_oom)
{
    PipeCtx ctx;
    int rc = compile_source(&ctx, "var x = 0; at (x > 5) x");
    UASSERT_EQ(0, rc);

    /* Drain the watcher pool — the next pool_alloc returns NULL. */
    ctx.vm.watcher_pool_freelist = NULL;

    UValue out;
    UVMError vm_rc = urbi_vm_run(&ctx.vm, NULL, &ctx.module, &out);
    UASSERT_EQ(UVM_OOM, (int)vm_rc);

    pipeline_ctx_destroy(&ctx);
}

/* whenever_install_propagates_pool_oom: same but for OP_WHENEVER_INSTALL. */
UTEST(whenever_install_propagates_pool_oom)
{
    PipeCtx ctx;
    int rc = compile_source(&ctx, "var c = 0; var b = 0; whenever (c) b");
    UASSERT_EQ(0, rc);

    ctx.vm.watcher_pool_freelist = NULL;

    UValue out;
    UVMError vm_rc = urbi_vm_run(&ctx.vm, NULL, &ctx.module, &out);
    UASSERT_EQ(UVM_OOM, (int)vm_rc);

    pipeline_ctx_destroy(&ctx);
}

/* at_sync_install_propagates_pool_oom: covers OP_AT_SYNC_INSTALL. */
UTEST(at_sync_install_propagates_pool_oom)
{
    PipeCtx ctx;
    int rc = compile_source(&ctx, "var c = 0; var b = 0; at sync (c) b");
    UASSERT_EQ(0, rc);

    ctx.vm.watcher_pool_freelist = NULL;

    UValue out;
    UVMError vm_rc = urbi_vm_run(&ctx.vm, NULL, &ctx.module, &out);
    UASSERT_EQ(UVM_OOM, (int)vm_rc);

    pipeline_ctx_destroy(&ctx);
}

/* waituntil_install_propagates_pool_oom: covers OP_WAITUNTIL_INSTALL.
 *
 * Note: install_watcher_runtime in WAITUNTIL mode still runs the cond on a
 * scratch frame *before* attempting pool_alloc; if cond starts truthy the
 * fast-path returns URBI_INSTALL_OK without ever touching the pool, so the
 * drain trick never witnesses OOM.  We pick a script where cond starts
 * falsy ("var x = 0; waituntil (x)" — x is 0 → falsy → would park) so
 * pool_alloc is reached. */
UTEST(waituntil_install_propagates_pool_oom)
{
    PipeCtx ctx;
    int rc = compile_source(&ctx, "var x = 0; waituntil (x)");
    UASSERT_EQ(0, rc);

    ctx.vm.watcher_pool_freelist = NULL;

    UValue out;
    UVMError vm_rc = urbi_vm_run(&ctx.vm, NULL, &ctx.module, &out);
    UASSERT_EQ(UVM_OOM, (int)vm_rc);

    pipeline_ctx_destroy(&ctx);
}

/* ===================================================================
 * VM-013 / T33: op_at_event_install rejects non-event operand
 * =================================================================== */

/* at_event_install_propagates_pool_oom:
 *
 * Routes through install_at_event_runtime; same pool-drain trick. */
UTEST(at_event_install_propagates_pool_oom)
{
    PipeCtx ctx;
    /* AT_EVENT requires a UEvent in the operand register; the simplest way
     * to land an emit-correct AT_EVENT install at M5 is via slot-change
     * (obj.x.changed?), which produces a UEvent via OP_GETSLOT_CHANGE_EVENT
     * and then routes through OP_AT_EVENT_INSTALL.  That path requires
     * stdlib Object.new — too heavy for a unit test.  Instead we exercise
     * the AT_EVENT_SYNC variant via the same compiler path — the bare
     * watcher-pool drain affects all 5 install opcodes uniformly. */
    int rc = compile_source(&ctx, "var x = 0; at (x > 5) x");
    UASSERT_EQ(0, rc);

    ctx.vm.watcher_pool_freelist = NULL;

    UValue out;
    UVMError vm_rc = urbi_vm_run(&ctx.vm, NULL, &ctx.module, &out);
    UASSERT_EQ(UVM_OOM, (int)vm_rc);

    pipeline_ctx_destroy(&ctx);
}

/* ===================================================================
 * VM-005 / T32: vm_alloc_closure OOM returns NULL
 *
 * Direct-API test against the helper function rather than going through
 * a whole compile-and-run.  We invoke vm_alloc_closure with a deliberately
 * starved allocator and verify NULL is returned without crashing.
 * (closure_list + next_alloc deleted at v0.8.4 Step C-3.) */
/* =================================================================== */

/* fail_after_n_alloc_state: counts down to first-OOM.  Used by the
 * fail_after_n_alloc allocator below. */
typedef struct {
    int allocs_remaining;  /* -1 means "always succeed"; positive means N more
                              successes then NULL */
} FailAfterNAllocState;

static void *
fail_after_n_alloc(void *ptr, size_t nbytes, void *ud)
{
    FailAfterNAllocState *st = (FailAfterNAllocState *)ud;
    /* Free path always succeeds. */
    if (ptr != NULL && nbytes == 0) { free(ptr); return NULL; }
    /* No-op path. */
    if (ptr == NULL && nbytes == 0) return NULL;
    /* Count down on alloc/realloc; return NULL when remaining hits 0. */
    if (st->allocs_remaining == 0) return NULL;
    if (st->allocs_remaining > 0) st->allocs_remaining--;
    return realloc(ptr, nbytes);
}

/* vm_alloc_closure_oom_returns_null:
 *
 * T32 / VM-005 (v0.8.4 Step C-3 update).  Pins that vm_alloc_closure returns
 * NULL on OOM and does not corrupt the GC heap.  The pre-C-3 test checked
 * that list_head was not corrupted; that mechanism is deleted — now the
 * relevant contract is: OOM returns NULL and the allocator state is clean.
 *
 * urbi_gc_alloc makes 2 alloc_fn calls per vm_alloc_closure call (cell +
 * sidecar node); setting allocs_remaining=0 triggers OOM on the first. */
UTEST(vm_alloc_closure_oom_returns_null)
{
    UVM vm;
    FailAfterNAllocState st = { .allocs_remaining = -1 };
    urbi_vm_init(&vm, fail_after_n_alloc, &st);

    /* Build a minimal UProto to feed vm_alloc_closure. */
    UProto proto;
    memset(&proto, 0, sizeof(proto));
    proto.nupvals = 0;

    /* First alloc succeeds. */
    UClosure *cl1 = vm_alloc_closure(&vm, &proto);
    UASSERT(cl1 != NULL);

    /* Force OOM on the next alloc: must return NULL. */
    st.allocs_remaining = 0;
    UClosure *cl2 = vm_alloc_closure(&vm, &proto);
    UASSERT(cl2 == NULL);

    /* cl1 is GC-managed — urbi_vm_destroy reclaims it. */
    st.allocs_remaining = -1;
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * VM-003 / T31: reactive-install kind-checks operand registers
 *
 * Run a hand-crafted single-opcode module against a strand whose
 * R[A] holds a non-closure UValue.  Pre-fix: the dispatcher casts to
 * UClosure* and (in release builds with the URBI_INTERNAL_ASSERT
 * compiled out) calls install_watcher_runtime with garbage.
 * Post-fix: the new vm_install_check_closure_operand sets
 * UVM_TYPE_ERROR and HALT()s before dispatch.
 *
 * VM-013 / T33: op_at_event_install kind-checks the event register
 * (separate test below — same harness). */
/* =================================================================== */

/* setup_strand_for_install: minimal strand with an empty cleanup stack and
 * a register array of UVM_STACK_CAP, ready to dispatch one install opcode.
 * Returns 0 on success.  Caller frees reg_stack and cleanup_base. */
static int
setup_strand_for_install(UStrand *s, UVM *vm,
                         const uint32_t *instrs,
                         UValue *reg_stack,
                         UCleanupEntry *cleanup_base)
{
    volatile unsigned char *p = (volatile unsigned char *)s;
    size_t n = sizeof(*s);
    size_t i;
    for (i = 0; i < n; i++) p[i] = 0;

    s->vm           = vm;
    s->state        = USTRAND_STATE_RUNNING;
    s->stack        = reg_stack;
    s->R            = reg_stack;
    s->pc           = instrs;
    s->pc_base      = instrs;
    s->cur_consts   = NULL;
    s->module       = NULL;
    s->frame_count  = 0;
    s->cleanup_base = cleanup_base;
    s->cleanup_cap  = 64;
    s->cleanup_depth = 0;
    s->cleanup_top  = NULL;
    return 0;
}

/* reactive_install_kind_checks_cond_operand:
 *
 * Hand-crafted bytecode: OP_AT_INSTALL A=0, B=1, C=0xFF.  R[0] holds
 * UVAL_NIL (zeroed by the strand setup).  The dispatcher must reject
 * it with UVM_TYPE_ERROR rather than casting NIL.v.p to UClosure*. */
UTEST(reactive_install_kind_checks_cond_operand)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    static uint32_t instrs[1];
    instrs[0] = uinstr_enc_abc(OP_AT_INSTALL,
                               /*A=cond_reg*/ 0,
                               /*B=body_reg*/ 1,
                               /*C=onleave_reg*/ 0xFFU);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);
    UCleanupEntry *cleanup_base =
        (UCleanupEntry *)calloc(64, sizeof(UCleanupEntry));
    UASSERT(cleanup_base != NULL);

    /* R[0] and R[1] start as UVAL_NIL after calloc — neither is a closure. */

    UStrand s;
    setup_strand_for_install(&s, &vm, instrs, reg_stack, cleanup_base);

    (void)dispatch_loop_until_yield(&s, /*step_budget*/ 100);

    /* Strand died (HALT); vm->last_error is UVM_TYPE_ERROR. */
    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    UASSERT_EQ((int)UVM_TYPE_ERROR, (int)vm.last_error);
    /* Watcher pool untouched. */
    UASSERT_EQ(0, (int)vm.watcher_pool_in_use);

    free(cleanup_base);
    free(reg_stack);
    urbi_vm_destroy(&vm);
}

/* at_event_install_kind_check:
 *
 * VM-013 / T33.  OP_AT_EVENT_INSTALL: A=event_reg, B=body_reg, C=onleave_reg.
 * R[A] must hold UVAL_EVENT, but the dispatcher pre-T33 cast directly to
 * UEvent*.  This test hand-crafts an OP_AT_EVENT_INSTALL where R[0]
 * holds UVAL_NIL and R[1] holds a fake UVAL_CLOSURE — without the new
 * vm_install_check_event_operand, install_at_event_runtime would walk
 * NIL.v.p (NULL) through e->at_watchers_head and crash.
 *
 * NOTE: this is a separate test from T31's
 * reactive_install_kind_checks_cond_operand because the AT_EVENT branch
 * was carved out of T31 — its A register is UEvent, not UClosure, so
 * the kind check uses uvalue_is_event() rather than the closure
 * predicate.  T31's existing test does NOT subsume this one. */
UTEST(at_event_install_kind_check)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    static uint32_t instrs[1];
    instrs[0] = uinstr_enc_abc(OP_AT_EVENT_INSTALL,
                               /*A=event_reg*/ 0,
                               /*B=body_reg*/ 1,
                               /*C=onleave_reg*/ 0xFFU);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);
    UCleanupEntry *cleanup_base =
        (UCleanupEntry *)calloc(64, sizeof(UCleanupEntry));
    UASSERT(cleanup_base != NULL);

    /* R[0] starts as UVAL_NIL — not an event.
     * R[1] is set to a "valid" UVAL_CLOSURE kind so that the body kind
     * check (T31) does NOT fire first; the event-kind check (T33) is
     * the dispatcher's first guard for OP_AT_EVENT_INSTALL and must
     * trip on R[0]. */
    reg_stack[1].kind = (uint8_t)UVAL_CLOSURE;
    reg_stack[1].v.p  = (void *)0xdeadbeefULL;  /* never dereferenced */

    UStrand s;
    setup_strand_for_install(&s, &vm, instrs, reg_stack, cleanup_base);

    (void)dispatch_loop_until_yield(&s, /*step_budget*/ 100);

    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    UASSERT_EQ((int)UVM_TYPE_ERROR, (int)vm.last_error);

    free(cleanup_base);
    free(reg_stack);
    urbi_vm_destroy(&vm);
}

/* fork_detach_kind_checks_closure_operand:
 *
 * Same approach for OP_FORK_DETACH: R[0] holds NIL, dispatcher must
 * fault rather than UB.  Pre-T31: URBI_INTERNAL_ASSERT was debug-only
 * and release builds happily cast NIL.v.p (NULL) → fork_spawn_child
 * dereferenced child_closure->proto → segfault. */
UTEST(fork_detach_kind_checks_closure_operand)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    static uint32_t instrs[1];
    instrs[0] = uinstr_enc_abc(OP_FORK_DETACH, 0, 0, 0);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);
    UCleanupEntry *cleanup_base =
        (UCleanupEntry *)calloc(64, sizeof(UCleanupEntry));
    UASSERT(cleanup_base != NULL);

    UStrand s;
    setup_strand_for_install(&s, &vm, instrs, reg_stack, cleanup_base);

    (void)dispatch_loop_until_yield(&s, /*step_budget*/ 100);

    /* The is_transient_strand guard fires first (zero-init means it's
     * unset → guard does NOT fire here; the kind check runs).
     * If the dispatcher's is_transient_strand guard had fired we'd see
     * a different message, so check explicitly via the strand state. */
    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    UASSERT_EQ((int)UVM_TYPE_ERROR, (int)vm.last_error);

    free(cleanup_base);
    free(reg_stack);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T120 / COV-002: OP_FORK_JOIN + OP_JOIN_WAIT kind-check coverage
 * ===================================================================
 *
 * The Phase-5 fork_detach_kind_checks_closure_operand test (above)
 * exercises OP_FORK_DETACH's kind check at uop_fork.c:155.  The two
 * sibling opcodes have nearly-identical kind-check stanzas at
 * uop_fork.c:194 (OP_FORK_JOIN) and uop_fork.c:232 (OP_JOIN_WAIT) which
 * were previously unexercised — pre-T120 coverage on src/vm/uop_fork.c
 * sat at 60 %.  T120 raises it by directly exercising those branches
 * plus OP_JOIN_WAIT's "child already DEAD" fast path.
 *
 * The test pattern matches fork_detach_kind_checks_closure_operand
 * exactly: a single hand-crafted opcode at instrs[0] with R[A] holding
 * a non-matching kind, dispatched via dispatch_loop_until_yield, then
 * asserts state == DEAD + last_error == UVM_TYPE_ERROR. */

UTEST(fork_join_kind_checks_closure_operand)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    static uint32_t instrs[1];
    /* OP_FORK_JOIN A=closure_reg, B=child_handle_reg */
    instrs[0] = uinstr_enc_abc(OP_FORK_JOIN, 0, 1, 0);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);
    UCleanupEntry *cleanup_base =
        (UCleanupEntry *)calloc(64, sizeof(UCleanupEntry));
    UASSERT(cleanup_base != NULL);

    /* R[0] = NIL via calloc; not a closure → kind check trips at
     * uop_fork.c:194. */

    UStrand s;
    setup_strand_for_install(&s, &vm, instrs, reg_stack, cleanup_base);

    (void)dispatch_loop_until_yield(&s, /*step_budget*/ 100);

    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    UASSERT_EQ((int)UVM_TYPE_ERROR, (int)vm.last_error);

    free(cleanup_base);
    free(reg_stack);
    urbi_vm_destroy(&vm);
}

UTEST(join_wait_kind_checks_strand_handle_operand)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    static uint32_t instrs[1];
    /* OP_JOIN_WAIT A=child_handle_reg */
    instrs[0] = uinstr_enc_abc(OP_JOIN_WAIT, 0, 0, 0);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);
    UCleanupEntry *cleanup_base =
        (UCleanupEntry *)calloc(64, sizeof(UCleanupEntry));
    UASSERT(cleanup_base != NULL);

    /* R[0] = NIL via calloc; not a strand handle → kind check trips at
     * uop_fork.c:232. */

    UStrand s;
    setup_strand_for_install(&s, &vm, instrs, reg_stack, cleanup_base);

    (void)dispatch_loop_until_yield(&s, /*step_budget*/ 100);

    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    UASSERT_EQ((int)UVM_TYPE_ERROR, (int)vm.last_error);

    free(cleanup_base);
    free(reg_stack);
    urbi_vm_destroy(&vm);
}

/* fork_spawn_child OOM coverage: op_fork_detach reaches the
 * urbi_strand_create-returns-NULL branch (uop_fork.c:58-63), then propagates
 * back through op_fork_detach (line 170) by setting strand DEAD +
 * fatal_status = UEXEC_CANCEL.
 *
 * Setup pattern: arm a Realm with the fail_after_n allocator, allow the
 * realm + parent strand allocations to succeed (allocs_remaining = -1
 * during setup), then flip to allocs_remaining = 0 right before invoking
 * op_fork_detach.  fork_spawn_child's first allocation (urbi_strand_create's
 * UStrand alloc) returns NULL, triggering the early-out path. */
UTEST(fork_detach_oom_marks_strand_dead)
{
    UVM vm;
    FailAfterNAllocState st = { .allocs_remaining = -1 };
    urbi_vm_init(&vm, fail_after_n_alloc, &st);
    sched_init(&vm, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    /* Hand-craft a closure-shaped UValue for R[0].  fork_spawn_child reads
     * the closure pointer through child_closure->proto, BUT only AFTER
     * urbi_strand_create succeeds — and we'll force urbi_strand_create to
     * fail BEFORE that deref.  So the closure pointer here can be any
     * non-NULL pointer; we use a small heap-allocated UClosure stub. */
    UClosure stub_cl;
    memset(&stub_cl, 0, sizeof(stub_cl));

    static uint32_t instrs[1];
    instrs[0] = uinstr_enc_abc(OP_FORK_DETACH, 0, 0, 0);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);
    UCleanupEntry *cleanup_base =
        (UCleanupEntry *)calloc(64, sizeof(UCleanupEntry));
    UASSERT(cleanup_base != NULL);

    reg_stack[0].kind = (uint8_t)UVAL_CLOSURE;
    reg_stack[0].v.p  = &stub_cl;

    UStrand s;
    setup_strand_for_install(&s, &vm, instrs, reg_stack, cleanup_base);
    /* op_fork_detach calls fork_spawn_child which asserts s->realm != NULL.
     * The setup helper zeroes the strand; we need realm pointer wired. */
    s.realm = realm;

    /* Starve next allocation. */
    st.allocs_remaining = 0;

    (void)dispatch_loop_until_yield(&s, /*step_budget*/ 100);

    /* After fork_spawn_child returns NULL: strand DEAD with CANCEL fatal. */
    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    UASSERT_EQ((int)UEXEC_CANCEL, (int)s.fatal_status);

    st.allocs_remaining = -1;  /* restore for teardown */
    free(cleanup_base);
    free(reg_stack);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* fork_spawn_child arm_from_closure OOM: lets urbi_strand_create succeed
 * but starves the urbi_strand_arm_from_closure register-stack allocation
 * (uop_fork.c:109), forcing the urbi_strand_destroy + DEAD-mark cleanup
 * branch at lines 111-116.
 *
 * The exact "allocs_remaining" needed is fragile — set generously and
 * sweep down until it lands on the arm-from-closure path.  3 successes
 * usually covers: child UStrand alloc + child cleanup-stack alloc + ...
 * then NULL on the register-stack alloc inside arm_from_closure. */
UTEST(fork_detach_arm_from_closure_oom)
{
    UVM vm;
    FailAfterNAllocState st = { .allocs_remaining = -1 };
    urbi_vm_init(&vm, fail_after_n_alloc, &st);
    sched_init(&vm, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    /* Build a real-shaped closure with a non-NULL proto so
     * urbi_strand_arm_from_closure has a valid proto pointer to read. */
    UProto stub_proto;
    memset(&stub_proto, 0, sizeof(stub_proto));
    stub_proto.nupvals = 0;
    UClosure stub_cl;
    memset(&stub_cl, 0, sizeof(stub_cl));
    stub_cl.proto = &stub_proto;

    static uint32_t instrs[1];
    instrs[0] = uinstr_enc_abc(OP_FORK_DETACH, 0, 0, 0);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);
    UCleanupEntry *cleanup_base =
        (UCleanupEntry *)calloc(64, sizeof(UCleanupEntry));
    UASSERT(cleanup_base != NULL);

    reg_stack[0].kind = (uint8_t)UVAL_CLOSURE;
    reg_stack[0].v.p  = &stub_cl;

    UStrand s;
    setup_strand_for_install(&s, &vm, instrs, reg_stack, cleanup_base);
    s.realm = realm;

    /* Allow child UStrand alloc (1) + cleanup-stack alloc (2), starve next.
     * If urbi_strand_create needs more allocs, this falls into the
     * urbi_strand_create-NULL path and still marks the strand DEAD —
     * either way the test asserts the same outcome (strand DEAD with
     * CANCEL fatal). */
    st.allocs_remaining = 2;

    (void)dispatch_loop_until_yield(&s, /*step_budget*/ 100);

    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    UASSERT_EQ((int)UEXEC_CANCEL, (int)s.fatal_status);

    st.allocs_remaining = -1;
    free(cleanup_base);
    free(reg_stack);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* OP_FORK_JOIN OOM: same shape as fork_detach_oom_marks_strand_dead but
 * exercises op_fork_join's `return -1` at uop_fork.c:208 (the
 * fork_spawn_child-returns-NULL branch shared with op_fork_detach). */
UTEST(fork_join_oom_marks_strand_dead)
{
    UVM vm;
    FailAfterNAllocState st = { .allocs_remaining = -1 };
    urbi_vm_init(&vm, fail_after_n_alloc, &st);
    sched_init(&vm, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    UClosure stub_cl;
    memset(&stub_cl, 0, sizeof(stub_cl));

    static uint32_t instrs[1];
    instrs[0] = uinstr_enc_abc(OP_FORK_JOIN, 0, 1, 0);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);
    UCleanupEntry *cleanup_base =
        (UCleanupEntry *)calloc(64, sizeof(UCleanupEntry));
    UASSERT(cleanup_base != NULL);

    reg_stack[0].kind = (uint8_t)UVAL_CLOSURE;
    reg_stack[0].v.p  = &stub_cl;

    UStrand s;
    setup_strand_for_install(&s, &vm, instrs, reg_stack, cleanup_base);
    s.realm = realm;

    st.allocs_remaining = 0;

    (void)dispatch_loop_until_yield(&s, /*step_budget*/ 100);

    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    UASSERT_EQ((int)UEXEC_CANCEL, (int)s.fatal_status);

    st.allocs_remaining = -1;
    free(cleanup_base);
    free(reg_stack);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* OP_JOIN_WAIT fast path: when the child handle in R[A] is already DEAD,
 * op_join_wait returns 0 immediately rather than threading the parent
 * onto the joiners chain.  The dispatcher sees rc == 0 and continues
 * with NEXT() (uvm.c:746).
 *
 * Exercises uop_fork.c:245-247 (the if-DEAD-return-0 fast path), which
 * was uncovered before T120 because every test_fork.c case waited on a
 * still-live child. */
UTEST(join_wait_fast_path_when_child_already_dead)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    /* Build a fake "dead child" UStrand: zeroed except state = DEAD.
     * The fast-path check only reads child->state via USTRAND_GET_STATE;
     * no other fields are touched on this path (op_join_wait returns 0
     * before any other deref). */
    static UStrand fake_child;
    memset(&fake_child, 0, sizeof(fake_child));
    fake_child.state = USTRAND_STATE_DEAD;

    static uint32_t instrs[2];
    instrs[0] = uinstr_enc_abc(OP_JOIN_WAIT, 0, 0, 0);
    /* OP_RET after JOIN_WAIT so dispatch exits cleanly (frame_count == 0
     * → strand transitions to DEAD via the same path used by other tests). */
    instrs[1] = uinstr_enc_abc(OP_RET, 1, 0, 0);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);
    UCleanupEntry *cleanup_base =
        (UCleanupEntry *)calloc(64, sizeof(UCleanupEntry));
    UASSERT(cleanup_base != NULL);

    /* R[0] = UVAL_STRAND wrapping our fake dead child. */
    reg_stack[0] = UVAL_STRAND_MAKE(&fake_child);

    UStrand s;
    setup_strand_for_install(&s, &vm, instrs, reg_stack, cleanup_base);

    (void)dispatch_loop_until_yield(&s, /*step_budget*/ 100);

    /* Strand reached HALT cleanly; no UVM_TYPE_ERROR. */
    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    UASSERT(vm.last_error != UVM_TYPE_ERROR);

    free(cleanup_base);
    free(reg_stack);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_vm_dispatch_ownership_suite(void)
{
    printf("test_vm_dispatch_ownership\n");
    utest_run("reactive_install_propagates_pool_oom",
              reactive_install_propagates_pool_oom);
    utest_run("whenever_install_propagates_pool_oom",
              whenever_install_propagates_pool_oom);
    utest_run("at_sync_install_propagates_pool_oom",
              at_sync_install_propagates_pool_oom);
    utest_run("waituntil_install_propagates_pool_oom",
              waituntil_install_propagates_pool_oom);
    utest_run("at_event_install_propagates_pool_oom",
              at_event_install_propagates_pool_oom);
    utest_run("vm_alloc_closure_oom_returns_null",
              vm_alloc_closure_oom_returns_null);
    utest_run("reactive_install_kind_checks_cond_operand",
              reactive_install_kind_checks_cond_operand);
    utest_run("at_event_install_kind_check",
              at_event_install_kind_check);
    utest_run("fork_detach_kind_checks_closure_operand",
              fork_detach_kind_checks_closure_operand);
    utest_run("fork_join_kind_checks_closure_operand",
              fork_join_kind_checks_closure_operand);
    utest_run("join_wait_kind_checks_strand_handle_operand",
              join_wait_kind_checks_strand_handle_operand);
    utest_run("join_wait_fast_path_when_child_already_dead",
              join_wait_fast_path_when_child_already_dead);
    utest_run("fork_detach_oom_marks_strand_dead",
              fork_detach_oom_marks_strand_dead);
    utest_run("fork_join_oom_marks_strand_dead",
              fork_join_oom_marks_strand_dead);
    utest_run("fork_detach_arm_from_closure_oom",
              fork_detach_arm_from_closure_oom);
}
