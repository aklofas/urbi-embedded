/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: refactor-3 SCHED-08 — independent block/freeze suspension
 * gates + tag-stop/cancel resume of SUSPENDED members (v0.13.3 Task 4).
 *
 * The pre-fix machinery (v0.10.9 W3a-W3c) encoded "why is this strand
 * SUSPENDED" in the single shared reason nibble, so:
 *   - block -> freeze -> unblock RAN the strand (should stay frozen) and
 *     the symmetric freeze -> block -> unfreeze order too;
 *   - freeze of a WAITING member silently no-op'd, so a sleeping member
 *     of a frozen tag woke and ran;
 *   - urbi_tag_stop on a SUSPENDED member deposited the unwind but never
 *     resumed the strand — it leaked forever (never ran its finally/
 *     onleave, kept strand_suspended_count pinned).
 *
 * Synthetic-strand cases mirror tests/unit/test_tag_state.c's harness;
 * the scripted end-to-end case mirrors tests/unit/test_tag_self_block.c. */

#include "utest.h"
#include "urbi/urbi.h"
#include "sched/ustrand.h"
#include "sched/usched_cooperative.h"
#include "runtime/ucleanup.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "tag/utag.h"
#include "chunk/uchunk.h"
#include "value/uarena.h"
#include "runtime/umacros.h"
#include "lex/ulex.h"
#include "parse/uast.h"
#include "parse/uparse.h"
#include "emit/uemit.h"

#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* --- Helpers (mirrors test_tag_state.c) --- */

static UValue
make_nil(void)
{
    UValue v;
    v.kind = UVAL_NIL;
    v.v.i  = 0;
    return v;
}

static UValue
make_int(int64_t i)
{
    UValue v;
    v.kind = UVAL_INT;
    v.v.i  = i;
    return v;
}

/* Minimal-strand harness: heap-alloc a register stack and zero a UStrand. */
static UValue *
strand_minimal(UStrand *s, UVM *vm)
{
    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    if (!reg_stack) return NULL;

    memset(s, 0, sizeof(*s));
    s->vm             = vm;
    s->state          = USTRAND_STATE_DORMANT;
    s->stack          = reg_stack;
    s->R              = reg_stack;
    s->pending_unwind = UEXEC_OK;
    s->fatal_status   = UEXEC_OK;

    strand_cleanup_stack_init(s, vm, (uint16_t)URBI_CLEANUP_MAX);
    return reg_stack;
}

/* Link strand to tag->member_strands_head via a synthetic TAG_SCOPE
 * cleanup entry — mirrors what OP_PUSH_TAG does at runtime. */
static void
link_strand_to_tag(UStrand *s, UTag *tag)
{
    UCleanupEntry *e = strand_cleanup_push(s);
    UASSERT(e != NULL);
    e->kind             = (uint8_t)UCLEANUP_TAG_SCOPE;
    e->flags            = 0U;
    e->handler_pc       = 0U;
    e->register_base    = 0U;
    e->register_count   = 0U;
    e->owning_tag       = tag;
    e->catch_pattern    = NULL;
    e->next_member      = tag->member_strands_head;
    e->strand_back      = s;
    tag->member_strands_head = e;
}

/* Common teardown: unlink the synthetic tag entry, free the harness. */
static void
teardown_member(UVM *vm, UStrand *s, UTag *tag, UValue *reg)
{
    tag->member_strands_head = NULL;
    free(reg);
    strand_cleanup_stack_destroy(s, vm);
    urbi_vm_destroy(vm);
}

/* ===== SCHED-08 matrix: gates are independent ===== */

/* block -> freeze -> unblock must leave the strand SUSPENDED (FREEZE gate
 * still set); unfreeze then resumes it with the BLOCK resume value still
 * staged in unblock_value.  Pre-fix: the shared reason nibble lets the
 * unblock-order game resume a still-gated strand. */
UTEST(block_freeze_unblock_stays_suspended)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);
    sched_strand_make_runnable(&s);

    UTag tag;
    memset(&tag, 0, sizeof(tag));
    tag.type_tag = 5U;  /* UTYPE_TAG */
    link_strand_to_tag(&s, &tag);

    UASSERT_EQ(urbi_tag_block(&vm, &tag, make_int(7)), URBI_OK);
    UASSERT_EQ(urbi_tag_freeze(&vm, &tag), URBI_OK);
    UASSERT_EQ(urbi_tag_unblock(&vm, &tag), URBI_OK);

    /* FREEZE gate still set: the strand must NOT resume. */
    UASSERT(USTRAND_IS_SUSPENDED(&s));
    UASSERT_EQ((int)USTRAND_GET_REASON(&s), (int)USTRAND_REASON_FREEZE);
    UASSERT_EQ(vm.strand_suspended_count, 1u);
    UASSERT_EQ(vm.strand_runnable_count, 0u);

    UASSERT_EQ(urbi_tag_unfreeze(&vm, &tag), URBI_OK);

    UASSERT(!USTRAND_IS_SUSPENDED(&s));
    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_READY);
    UASSERT_EQ(vm.strand_suspended_count, 0u);
    UASSERT_EQ(vm.strand_runnable_count, 1u);
    /* The BLOCK resume value survives to the actual resume. */
    UASSERT_EQ(s.unblock_value.kind, (uint8_t)UVAL_INT);
    UASSERT_EQ(s.unblock_value.v.i, (int64_t)7);

    teardown_member(&vm, &s, &tag, reg);
}

/* Symmetric order: freeze -> block -> unfreeze stays SUSPENDED (BLOCK gate
 * still set); unblock then resumes with the BLOCK resume value. */
UTEST(freeze_block_unfreeze_stays_suspended)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);
    sched_strand_make_runnable(&s);

    UTag tag;
    memset(&tag, 0, sizeof(tag));
    tag.type_tag = 5U;
    link_strand_to_tag(&s, &tag);

    UASSERT_EQ(urbi_tag_freeze(&vm, &tag), URBI_OK);
    UASSERT_EQ(urbi_tag_block(&vm, &tag, make_int(9)), URBI_OK);
    UASSERT_EQ(urbi_tag_unfreeze(&vm, &tag), URBI_OK);

    /* BLOCK gate still set: the strand must NOT resume. */
    UASSERT(USTRAND_IS_SUSPENDED(&s));
    UASSERT_EQ((int)USTRAND_GET_REASON(&s), (int)USTRAND_REASON_BLOCK);
    UASSERT_EQ(vm.strand_suspended_count, 1u);
    UASSERT_EQ(vm.strand_runnable_count, 0u);

    UASSERT_EQ(urbi_tag_unblock(&vm, &tag), URBI_OK);

    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_READY);
    UASSERT_EQ(vm.strand_suspended_count, 0u);
    UASSERT_EQ(vm.strand_runnable_count, 1u);
    UASSERT_EQ(s.unblock_value.kind, (uint8_t)UVAL_INT);
    UASSERT_EQ(s.unblock_value.v.i, (int64_t)9);

    teardown_member(&vm, &s, &tag, reg);
}

/* SCHED-08: freeze of a SLEEP-parked member must gate its timer wake: when
 * the timer fires the strand transitions to SUSPENDED, not READY.  Pre-fix:
 * the freeze silently no-ops on WAITING and the sleeper runs. */
UTEST(freeze_waiting_member_gates_wake)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);
    sched_strand_make_runnable(&s);

    UTag tag;
    memset(&tag, 0, sizeof(tag));
    tag.type_tag = 5U;
    link_strand_to_tag(&s, &tag);

    /* Park on the sleep queue (READY -> WAITING_SLEEP is a legal entry). */
    sched_strand_block(&s, USTRAND_REASON_SLEEP, 1000u);
    UASSERT(USTRAND_IS_WAITING(&s));
    UASSERT_EQ(vm.strand_waiting_count, 1u);

    /* Freeze the tag: the parked member stays parked (the gate does the
     * work at wake time), still counted WAITING. */
    UASSERT_EQ(urbi_tag_freeze(&vm, &tag), URBI_OK);
    UASSERT(USTRAND_IS_WAITING(&s));
    UASSERT_EQ(vm.strand_waiting_count, 1u);
    UASSERT_EQ(vm.strand_suspended_count, 0u);

    /* Timer fires: the wake must land in SUSPENDED, not READY. */
    sched_strand_unblock(&s);
    UASSERT(USTRAND_IS_SUSPENDED(&s));
    UASSERT_EQ((int)USTRAND_GET_REASON(&s), (int)USTRAND_REASON_FREEZE);
    UASSERT_EQ(vm.strand_waiting_count, 0u);
    UASSERT_EQ(vm.strand_suspended_count, 1u);
    UASSERT_EQ(vm.strand_runnable_count, 0u);

    /* Unfreeze releases the gated wake. */
    UASSERT_EQ(urbi_tag_unfreeze(&vm, &tag), URBI_OK);
    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_READY);
    UASSERT_EQ(vm.strand_suspended_count, 0u);
    UASSERT_EQ(vm.strand_runnable_count, 1u);

    teardown_member(&vm, &s, &tag, reg);
}

/* freeze-of-WAITING + unfreeze-before-timer: the strand stays parked the
 * whole time and the timer later fires normally (ungated wake -> READY). */
UTEST(freeze_waiting_unfreeze_before_timer_stays_parked)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);
    sched_strand_make_runnable(&s);

    UTag tag;
    memset(&tag, 0, sizeof(tag));
    tag.type_tag = 5U;
    link_strand_to_tag(&s, &tag);

    sched_strand_block(&s, USTRAND_REASON_SLEEP, 1000u);
    UASSERT_EQ(urbi_tag_freeze(&vm, &tag), URBI_OK);

    /* Unfreeze while still parked: stays WAITING (gate cleared in place). */
    UASSERT_EQ(urbi_tag_unfreeze(&vm, &tag), URBI_OK);
    UASSERT(USTRAND_IS_WAITING(&s));
    UASSERT_EQ(vm.strand_waiting_count, 1u);
    UASSERT_EQ(vm.strand_suspended_count, 0u);

    /* Timer fires normally: ungated wake -> READY. */
    sched_strand_unblock(&s);
    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_READY);
    UASSERT_EQ(vm.strand_waiting_count, 0u);
    UASSERT_EQ(vm.strand_runnable_count, 1u);

    teardown_member(&vm, &s, &tag, reg);
}

/* ===== SCHED-08: tag-stop / cancel resume SUSPENDED members ===== */

/* urbi_tag_stop on a SUSPENDED member deposits the unwind AND resumes the
 * strand so it can consume it.  Pre-fix: deposit without resume -> the
 * member never runs its unwind and leaks forever. */
UTEST(tag_stop_wakes_suspended_member)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);
    sched_strand_make_runnable(&s);

    UTag tag;
    memset(&tag, 0, sizeof(tag));
    tag.type_tag = 5U;
    link_strand_to_tag(&s, &tag);

    UASSERT_EQ(urbi_tag_block(&vm, &tag, make_nil()), URBI_OK);
    UASSERT(USTRAND_IS_SUSPENDED(&s));
    UASSERT_EQ(vm.strand_suspended_count, 1u);

    UASSERT_EQ(urbi_tag_stop(&vm, &tag, make_nil()), URBI_OK);

    /* Stop overrides suspension: gates cleared, strand READY with the
     * TAG_STOP deposit pending. */
    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_READY);
    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_TAG_STOP);
    UASSERT_EQ((unsigned)s.suspend_gates, 0u);
    UASSERT_EQ(vm.strand_suspended_count, 0u);
    UASSERT_EQ(vm.strand_runnable_count, 1u);

    teardown_member(&vm, &s, &tag, reg);
}

/* Same shape for the doubly-gated case: stop must clear BOTH gates. */
UTEST(tag_stop_wakes_doubly_gated_member)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);
    sched_strand_make_runnable(&s);

    UTag tag;
    memset(&tag, 0, sizeof(tag));
    tag.type_tag = 5U;
    link_strand_to_tag(&s, &tag);

    UASSERT_EQ(urbi_tag_block(&vm, &tag, make_nil()), URBI_OK);
    UASSERT_EQ(urbi_tag_freeze(&vm, &tag), URBI_OK);

    UASSERT_EQ(urbi_tag_stop(&vm, &tag, make_nil()), URBI_OK);

    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_READY);
    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_TAG_STOP);
    UASSERT_EQ((unsigned)s.suspend_gates, 0u);
    UASSERT_EQ(vm.strand_suspended_count, 0u);

    teardown_member(&vm, &s, &tag, reg);
}

/* Freeze of a WAITING member followed by tag-stop: the stop wake must NOT
 * be gated into SUSPENDED — stop overrides the pending gate too. */
UTEST(tag_stop_overrides_gate_on_waiting_member)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);
    sched_strand_make_runnable(&s);

    UTag tag;
    memset(&tag, 0, sizeof(tag));
    tag.type_tag = 5U;
    link_strand_to_tag(&s, &tag);

    sched_strand_block(&s, USTRAND_REASON_SLEEP, 1000u);
    UASSERT_EQ(urbi_tag_freeze(&vm, &tag), URBI_OK);
    UASSERT(USTRAND_IS_WAITING(&s));

    UASSERT_EQ(urbi_tag_stop(&vm, &tag, make_nil()), URBI_OK);

    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_READY);
    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_TAG_STOP);
    UASSERT_EQ((unsigned)s.suspend_gates, 0u);
    UASSERT_EQ(vm.strand_waiting_count, 0u);
    UASSERT_EQ(vm.strand_suspended_count, 0u);
    UASSERT_EQ(vm.strand_runnable_count, 1u);

    teardown_member(&vm, &s, &tag, reg);
}

/* urbi_strand_cancel on a SUSPENDED target mirrors tag-stop (same leak
 * shape pre-fix: CANCEL deposited, strand never resumed). */
UTEST(strand_cancel_wakes_suspended_target)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);
    sched_strand_make_runnable(&s);

    UTag tag;
    memset(&tag, 0, sizeof(tag));
    tag.type_tag = 5U;
    link_strand_to_tag(&s, &tag);

    UASSERT_EQ(urbi_tag_block(&vm, &tag, make_nil()), URBI_OK);
    UASSERT(USTRAND_IS_SUSPENDED(&s));

    UASSERT_EQ(urbi_strand_cancel(&vm, &s, make_nil()), URBI_OK);

    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_READY);
    UASSERT_EQ((int)s.pending_unwind, (int)UEXEC_CANCEL);
    UASSERT_EQ((unsigned)s.suspend_gates, 0u);
    UASSERT_EQ(vm.strand_suspended_count, 0u);
    UASSERT_EQ(vm.strand_runnable_count, 1u);

    teardown_member(&vm, &s, &tag, reg);
}

/* ===================================================================
 * Scripted end-to-end: tag-stop on a self-blocked member runs its
 * finally and dies (boilerplate mirrors test_tag_self_block.c).
 * =================================================================== */

static UProto *
compile_heap_chunk(UVM *vm, const char *src)
{
    UProto *module = (UProto *)vm->alloc_fn(NULL, sizeof(UProto), vm->alloc_ud);
    if (module == NULL) return NULL;
    urbi_zero(module, sizeof(*module));
    module->heap_allocated = true;
    module->alloc_fn       = vm->alloc_fn;
    module->alloc_ud       = vm->alloc_ud;

    UArena arena;
    uarena_init(&arena, 4096);

    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UEmitter e;
    uemit_init(&e, module, &arena, vm, NULL);
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

    if (!ok) {
        uchunk_destroy(module, vm);
        return NULL;
    }
    return module;
}

static UStepResult
pump_steps(UVM *vm, int max_steps)
{
    UStepResult rc = URBI_STEP_RUNNING;
    for (int i = 0; i < max_steps; i++) {
        rc = urbi_step(vm, 1000, NULL);
        if (rc != URBI_STEP_RUNNING) break;
    }
    return rc;
}

static UStrand *
find_strand_in_realm(UVM *vm, const UStrand *needle)
{
    URealm *realm = urbi_realm_global(vm);
    for (UStrand *p = realm->strands_head; p != NULL; p = p->next_in_realm)
        if (p == needle) return p;
    return NULL;
}

static int64_t
read_int_global(UVM *vm, const char *name, size_t len)
{
    URealm *gr = urbi_realm_global(vm);
    UValue v = {0};
    if (urbi_realm_get_global(vm, gr, name, len, &v) != URBI_OK) return -1;
    if (v.kind != (uint8_t)UVAL_INT) return -1;
    return v.v.i;
}

/* SCHED-08: stop on a SUSPENDED member must make the deposit consumable —
 * the resumed strand runs its finally (fin9 == 1) and dies (eager-reaped),
 * leaving the VM quiescent with strand_suspended_count == 0.  Pre-fix the
 * strand leaked SUSPENDED forever: fin9 stayed 0, the counter stayed 1. */
UTEST(tag_stop_resumes_suspended_member_e2e)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *gr = urbi_realm_global(&vm);
    UASSERT(gr != NULL);
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, gr, "fin9", 4,
                                              urbi_make_int(0)));

    UProto *module = compile_heap_chunk(&vm,
        "var t = Tag.new(); t: { try { t.block(); 1 } finally { Realm.fin9 = 1 } }");
    UASSERT(module != NULL);

    URealm *realm = urbi_realm_global(&vm);
    UStrand *loader = urbi_strand_create_for_module(&vm, realm, module);
    UASSERT(loader != NULL);

    UStepResult rc = pump_steps(&vm, 100);

    /* Parked SUSPENDED on the self-block; finally not yet run. */
    UASSERT_EQ((int)URBI_STEP_QUIESCENT, (int)rc);
    UStrand *s = find_strand_in_realm(&vm, loader);
    UASSERT(s != NULL);
    if (s != NULL) {
        UASSERT(USTRAND_IS_SUSPENDED(s));
        UASSERT_EQ(vm.strand_suspended_count, 1u);
        UASSERT_EQ((int64_t)0, read_int_global(&vm, "fin9", 4));
        UASSERT(s->wait_payload.suspend_tag != NULL);

        /* Stop the tag: the SUSPENDED member must be resumed so the
         * TAG_STOP deposit is consumable. */
        UASSERT_EQ(URBI_OK, urbi_tag_stop(&vm, s->wait_payload.suspend_tag,
                                          urbi_make_nil()));

        rc = pump_steps(&vm, 100);

        /* The member ran its unwind (finally observable) and died. */
        UASSERT_EQ((int)URBI_STEP_QUIESCENT, (int)rc);
        UASSERT(vm.fatal_strand == NULL);
        UASSERT_EQ((int64_t)1, read_int_global(&vm, "fin9", 4));
        UASSERT_EQ(vm.strand_suspended_count, 0u);
        UASSERT(find_strand_in_realm(&vm, loader) == NULL);
    }

    uchunk_destroy(module, &vm);
    urbi_vm_destroy(&vm);
}

/* ===== Suite ===== */
void test_tag_gate_matrix_suite(void);

void
test_tag_gate_matrix_suite(void)
{
    utest_run("block_freeze_unblock_stays_suspended",
              block_freeze_unblock_stays_suspended);
    utest_run("freeze_block_unfreeze_stays_suspended",
              freeze_block_unfreeze_stays_suspended);
    utest_run("freeze_waiting_member_gates_wake",
              freeze_waiting_member_gates_wake);
    utest_run("freeze_waiting_unfreeze_before_timer_stays_parked",
              freeze_waiting_unfreeze_before_timer_stays_parked);
    utest_run("tag_stop_wakes_suspended_member",
              tag_stop_wakes_suspended_member);
    utest_run("tag_stop_wakes_doubly_gated_member",
              tag_stop_wakes_doubly_gated_member);
    utest_run("tag_stop_overrides_gate_on_waiting_member",
              tag_stop_overrides_gate_on_waiting_member);
    utest_run("strand_cancel_wakes_suspended_target",
              strand_cancel_wakes_suspended_target);
    utest_run("tag_stop_resumes_suspended_member_e2e",
              tag_stop_resumes_suspended_member_e2e);
}
