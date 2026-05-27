/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: W3a/W3b/W3c — SUSPENDED scheduler state + tag.block/freeze
 * (v0.10.9-tag-state).
 *
 * Tests cover:
 *  W3a:
 *    1. urbi_strand_suspend(REASON_BLOCK) transitions READY → SUSPENDED_BLOCK
 *       and removes the strand from the cooperative ready queue.
 *    2. urbi_strand_resume returns SUSPENDED → READY and re-enqueues.
 *    3. urbi_strand_suspend is a no-op for DEAD/DORMANT/WAITING strands.
 *    4. urbi_strand_suspend(REASON_FREEZE) is independent of REASON_BLOCK.
 *    5. urbi_strand_suspend / _resume accept NULL strand (no crash).
 *
 *  W3b:
 *    6. urbi_tag_block sets UTAG_FLAG_BLOCKED and suspends member strands.
 *    7. urbi_tag_unblock clears UTAG_FLAG_BLOCKED and resumes BLOCK-suspended
 *       members but leaves FREEZE-suspended members alone.
 *    8. urbi_tag_block / _unblock validate NULL args.
 *
 *  W3c:
 *    9. urbi_tag_freeze sets UTAG_FLAG_FROZEN and suspends member strands.
 *   10. urbi_tag_unfreeze clears UTAG_FLAG_FROZEN and resumes FREEZE-suspended
 *       members; BLOCK-suspended members stay suspended.
 *
 * Mirrors tests/unit/test_capi_unwind.c's minimal-strand harness pattern.
 */

#include "utest.h"
#include "urbi/urbi.h"
#include "sched/ustrand.h"
#include "sched/usched_cooperative.h"
#include "runtime/ucleanup.h"
#include "vm/uvm.h"
#include "tag/utag.h"

#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* --- Helpers --- */

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

/* Minimal-strand harness: heap-alloc a register stack and zero a UStrand.
 * Mirrors tests/unit/test_capi_unwind.c's strand_minimal pattern. */
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

/* ===== Tests ===== */

/* W3a: urbi_strand_suspend on READY strand transitions to SUSPENDED_BLOCK and
 * removes the strand from the ready queue. */
UTEST(suspend_ready_to_suspended_block)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);
    sched_strand_make_runnable(&s);
    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_READY);
    UASSERT_EQ(vm.strand_runnable_count, 1u);

    UTag tag;
    memset(&tag, 0, sizeof(tag));
    tag.type_tag = 5U;  /* UTYPE_TAG */

    urbi_strand_suspend(&s, USTRAND_REASON_BLOCK, &tag);

    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_SUSPENDED);
    UASSERT_EQ((int)USTRAND_GET_REASON(&s), (int)USTRAND_REASON_BLOCK);
    UASSERT(s.wait_payload.suspend_tag == &tag);
    UASSERT_EQ(vm.strand_runnable_count, 0u);

    free(reg);
    strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* W3a: urbi_strand_resume restores SUSPENDED → READY and re-enqueues. */
UTEST(resume_suspended_to_ready)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);
    sched_strand_make_runnable(&s);

    UTag tag;
    memset(&tag, 0, sizeof(tag));
    tag.type_tag = 5U;

    urbi_strand_suspend(&s, USTRAND_REASON_BLOCK, &tag);
    UASSERT_EQ(vm.strand_runnable_count, 0u);

    urbi_strand_resume(&s, make_int(42));

    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_READY);
    UASSERT_EQ(vm.strand_runnable_count, 1u);
    UASSERT_EQ(s.unblock_value.kind, (uint8_t)UVAL_INT);
    UASSERT_EQ(s.unblock_value.v.i, (int64_t)42);
    UASSERT(s.wait_payload.suspend_tag == NULL);

    free(reg);
    strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* W3a: suspend is a no-op for DEAD strands (defensive). */
UTEST(suspend_dead_strand_is_noop)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);
    s.state = USTRAND_STATE_DEAD;

    UTag tag;
    memset(&tag, 0, sizeof(tag));
    tag.type_tag = 5U;

    urbi_strand_suspend(&s, USTRAND_REASON_BLOCK, &tag);

    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_DEAD);

    free(reg);
    strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* W3a: REASON_FREEZE and REASON_BLOCK are distinct sub-codes. */
UTEST(suspend_freeze_distinct_from_block)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);
    sched_strand_make_runnable(&s);

    UTag tag;
    memset(&tag, 0, sizeof(tag));
    tag.type_tag = 5U;

    urbi_strand_suspend(&s, USTRAND_REASON_FREEZE, &tag);

    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_SUSPENDED);
    UASSERT_EQ((int)USTRAND_GET_REASON(&s), (int)USTRAND_REASON_FREEZE);

    free(reg);
    strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* W3a: urbi_strand_suspend / _resume tolerate NULL strand (no crash). */
UTEST(suspend_resume_null_strand)
{
    UTag tag;
    memset(&tag, 0, sizeof(tag));
    tag.type_tag = 5U;

    /* Should not crash; behaviour is silent no-op. */
    urbi_strand_suspend(NULL, USTRAND_REASON_BLOCK, &tag);
    urbi_strand_resume(NULL, make_nil());

    /* Tag pointer NULL is also tolerated (silent no-op). */
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);
    UValue *reg = strand_minimal(&s, &vm);
    sched_strand_make_runnable(&s);

    urbi_strand_suspend(&s, USTRAND_REASON_BLOCK, NULL);
    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_READY);  /* unchanged */

    free(reg);
    strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
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

/* W3b: urbi_tag_block sets UTAG_FLAG_BLOCKED and suspends each member strand. */
UTEST(tag_block_sets_flag_and_suspends_members)
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

    int rc = urbi_tag_block(&vm, &tag, make_int(7));
    UASSERT_EQ(rc, URBI_OK);

    UASSERT((tag.flags & UTAG_FLAG_BLOCKED) != 0U);
    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_SUSPENDED);
    UASSERT_EQ((int)USTRAND_GET_REASON(&s), (int)USTRAND_REASON_BLOCK);
    UASSERT_EQ(s.unblock_value.kind, (uint8_t)UVAL_INT);
    UASSERT_EQ(s.unblock_value.v.i, (int64_t)7);

    /* Clear the synthetic tag-link so ustrand_destroy / strand_cleanup_stack_destroy
     * teardown does not assert on a non-empty member list. */
    tag.member_strands_head = NULL;

    free(reg);
    strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* W3b: urbi_tag_unblock clears flag and resumes only BLOCK-suspended members. */
UTEST(tag_unblock_clears_flag_and_resumes_block_members)
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
    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_SUSPENDED);

    UASSERT_EQ(urbi_tag_unblock(&vm, &tag), URBI_OK);

    UASSERT((tag.flags & UTAG_FLAG_BLOCKED) == 0U);
    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_READY);

    tag.member_strands_head = NULL;
    free(reg);
    strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* W3b/W3c: unblock does NOT touch FREEZE-suspended strands. */
UTEST(tag_unblock_leaves_freeze_suspended_alone)
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

    /* Freeze first, then unblock — must be a no-op on the strand. */
    urbi_strand_suspend(&s, USTRAND_REASON_FREEZE, &tag);
    UASSERT_EQ((int)USTRAND_GET_REASON(&s), (int)USTRAND_REASON_FREEZE);

    UASSERT_EQ(urbi_tag_unblock(&vm, &tag), URBI_OK);

    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_SUSPENDED);
    UASSERT_EQ((int)USTRAND_GET_REASON(&s), (int)USTRAND_REASON_FREEZE);

    tag.member_strands_head = NULL;
    free(reg);
    strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* W3b: urbi_tag_block / _unblock validate NULL args. */
UTEST(tag_block_rejects_null_args)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UTag tag;
    memset(&tag, 0, sizeof(tag));
    tag.type_tag = 5U;

    UASSERT_EQ(urbi_tag_block(NULL, &tag, make_nil()), URBI_ERR_INVALID_ARG);
    UASSERT_EQ(urbi_tag_block(&vm, NULL, make_nil()), URBI_ERR_INVALID_ARG);
    UASSERT_EQ(urbi_tag_unblock(NULL, &tag), URBI_ERR_INVALID_ARG);
    UASSERT_EQ(urbi_tag_unblock(&vm, NULL), URBI_ERR_INVALID_ARG);

    urbi_vm_destroy(&vm);
}

/* W3c: urbi_tag_freeze sets UTAG_FLAG_FROZEN and suspends member strands. */
UTEST(tag_freeze_sets_flag_and_suspends_members)
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

    UASSERT((tag.flags & UTAG_FLAG_FROZEN) != 0U);
    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_SUSPENDED);
    UASSERT_EQ((int)USTRAND_GET_REASON(&s), (int)USTRAND_REASON_FREEZE);

    tag.member_strands_head = NULL;
    free(reg);
    strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* W3c: urbi_tag_unfreeze clears flag and resumes FREEZE-suspended members. */
UTEST(tag_unfreeze_clears_flag_and_resumes_freeze_members)
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
    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_SUSPENDED);

    UASSERT_EQ(urbi_tag_unfreeze(&vm, &tag), URBI_OK);

    UASSERT((tag.flags & UTAG_FLAG_FROZEN) == 0U);
    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_READY);

    tag.member_strands_head = NULL;
    free(reg);
    strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* W3c: unfreeze does NOT touch BLOCK-suspended strands. */
UTEST(tag_unfreeze_leaves_block_suspended_alone)
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

    urbi_strand_suspend(&s, USTRAND_REASON_BLOCK, &tag);
    UASSERT_EQ((int)USTRAND_GET_REASON(&s), (int)USTRAND_REASON_BLOCK);

    UASSERT_EQ(urbi_tag_unfreeze(&vm, &tag), URBI_OK);

    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_SUSPENDED);
    UASSERT_EQ((int)USTRAND_GET_REASON(&s), (int)USTRAND_REASON_BLOCK);

    tag.member_strands_head = NULL;
    free(reg);
    strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* W3c: urbi_tag_freeze / _unfreeze validate NULL args. */
UTEST(tag_freeze_rejects_null_args)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    UTag tag;
    memset(&tag, 0, sizeof(tag));
    tag.type_tag = 5U;

    UASSERT_EQ(urbi_tag_freeze(NULL, &tag), URBI_ERR_INVALID_ARG);
    UASSERT_EQ(urbi_tag_freeze(&vm, NULL), URBI_ERR_INVALID_ARG);
    UASSERT_EQ(urbi_tag_unfreeze(NULL, &tag), URBI_ERR_INVALID_ARG);
    UASSERT_EQ(urbi_tag_unfreeze(&vm, NULL), URBI_ERR_INVALID_ARG);

    urbi_vm_destroy(&vm);
}

/* ===== Suite ===== */
void test_tag_state_suite(void);

void
test_tag_state_suite(void)
{
    utest_run("suspend_ready_to_suspended_block",
              suspend_ready_to_suspended_block);
    utest_run("resume_suspended_to_ready",
              resume_suspended_to_ready);
    utest_run("suspend_dead_strand_is_noop",
              suspend_dead_strand_is_noop);
    utest_run("suspend_freeze_distinct_from_block",
              suspend_freeze_distinct_from_block);
    utest_run("suspend_resume_null_strand",
              suspend_resume_null_strand);
    utest_run("tag_block_sets_flag_and_suspends_members",
              tag_block_sets_flag_and_suspends_members);
    utest_run("tag_unblock_clears_flag_and_resumes_block_members",
              tag_unblock_clears_flag_and_resumes_block_members);
    utest_run("tag_unblock_leaves_freeze_suspended_alone",
              tag_unblock_leaves_freeze_suspended_alone);
    utest_run("tag_block_rejects_null_args",
              tag_block_rejects_null_args);
    utest_run("tag_freeze_sets_flag_and_suspends_members",
              tag_freeze_sets_flag_and_suspends_members);
    utest_run("tag_unfreeze_clears_flag_and_resumes_freeze_members",
              tag_unfreeze_clears_flag_and_resumes_freeze_members);
    utest_run("tag_unfreeze_leaves_block_suspended_alone",
              tag_unfreeze_leaves_block_suspended_alone);
    utest_run("tag_freeze_rejects_null_args",
              tag_freeze_rejects_null_args);
}
