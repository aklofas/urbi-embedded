/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: row 7 C API surface (T12 + T13).
 *
 * Tests cover:
 *  1. urbi_strand_cancel deposits CANCEL and reports it via unwind_status.
 *  2. urbi_strand_cancel on a DEAD strand returns URBI_ERR_STRAND_FATAL.
 *  3. urbi_strand_panic marks strand DEAD; urbi_strand_is_fatal confirms it.
 *  4. urbi_strand_reset clears fatal + unwind state and returns to DORMANT.
 *  5. urbi_strand_unwind_status returns UEXEC_OK for clean strand.
 *  6. urbi_throw deposits THROW; urbi_return_val deposits RETURN.
 *  7. urbi_tag_stop_local deposits TAG_STOP with target pointer.
 *  8. urbi_tag_stop accepts valid args (URBI_OK) and rejects NULLs.
 *  9. urbi_strand_cancel on a WAITING strand transitions it to READY (T13).
 * 10. urbi_sched_strand_cleanup_stack_init returns -1 when allocator returns NULL (T13). */

#include "utest.h"
#include "urbi/urbi.h"
#include "sched/ustrand.h"
#include "sched/usched_cooperative.h"  /* urbi_sched_strand_block (case 9 park) */
#include "runtime/ucleanup.h"
#include "runtime/uunwind.h"  /* urbi_unwind — internal walker, used by W2 D3 test */
#include "vm/uvm.h"
#include "chunk/uchunk.h"
#include "tag/utag.h"    /* UTag — needed for urbi_tag_stop real impl (T31) */
#include "realm/urealm.h" /* URealm — needed for tag-creation in W2 D3 test */

#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* --- Helper: make a nil UValue for use as a cancel/throw reason --- */
static UValue
make_nil(void)
{
    UValue v;
    v.kind = UVAL_NIL;
    v.v.i  = 0;
    return v;
}

/* --- Helper: make an integer UValue --- */
static UValue
make_int(int64_t i)
{
    UValue v;
    v.kind = UVAL_INT;
    v.v.i  = i;
    return v;
}

/* --- Helper: initialise a minimal UStrand for testing ---
 * Allocates the register stack on the heap; caller must free it.
 * Does NOT set up an instruction array — tests that don't dispatch don't need it. */
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

    urbi_sched_strand_cleanup_stack_init(s, vm, (uint16_t)URBI_CLEANUP_MAX);
    return reg_stack;
}

/* ===== Tests ===== */

/* 1. urbi_strand_cancel deposits CANCEL and is readable via unwind_status. */
UTEST(capi_strand_cancel_deposits_unwind)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);

    /* Precondition: clean strand. */
    UASSERT_EQ(urbi_strand_unwind_status(&vm, &s), UEXEC_OK);

    /* Act: deposit CANCEL. */
    int rc = urbi_strand_cancel(&vm, &s, make_nil());
    UASSERT_EQ(rc, URBI_OK);

    /* The unwind status must now be CANCEL. */
    UASSERT_EQ(urbi_strand_unwind_status(&vm, &s), UEXEC_CANCEL);

    free(reg);
    urbi_sched_strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* 2. urbi_strand_cancel on an already-DEAD strand returns URBI_ERR_STRAND_FATAL. */
UTEST(capi_strand_cancel_rejects_dead_strand)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);
    s.state = USTRAND_STATE_DEAD;

    int rc = urbi_strand_cancel(&vm, &s, make_nil());
    UASSERT_EQ(rc, URBI_ERR_STRAND_FATAL);

    free(reg);
    urbi_sched_strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* 3. urbi_strand_panic marks strand DEAD; urbi_strand_is_fatal confirms it. */
UTEST(capi_strand_panic_marks_fatal)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);

    int rc = urbi_strand_panic(&vm, &s, "host-error");
    UASSERT_EQ(rc, URBI_OK);

    /* The strand must be DEAD. */
    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_DEAD);

    /* urbi_strand_is_fatal must return true with the correct status. */
    UStrandUnwind status = URBI_UNWIND_OK;
    UValue val;
    val.kind = UVAL_NIL; val.v.i = 0;
    bool fatal = urbi_strand_is_fatal(&vm, &s, &status, &val);
    UASSERT(fatal);
    UASSERT_EQ(status, UEXEC_CANCEL);

    free(reg);
    urbi_sched_strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* 4. urbi_strand_reset clears fatal + unwind, returns strand to DORMANT. */
UTEST(capi_strand_reset_clears_fatal_and_returns_dormant)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);

    /* Panic the strand first. */
    urbi_strand_panic(&vm, &s, "boom");
    UASSERT(urbi_strand_is_fatal(&vm, &s, NULL, NULL));

    /* Reset: should clear all unwind state. */
    int rc = urbi_strand_reset(&vm, &s);
    UASSERT_EQ(rc, URBI_OK);

    /* No longer fatal. */
    UASSERT(!urbi_strand_is_fatal(&vm, &s, NULL, NULL));

    /* Back to DORMANT. */
    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_DORMANT);

    /* pending_unwind cleared. */
    UASSERT_EQ(urbi_strand_unwind_status(&vm, &s), UEXEC_OK);

    free(reg);
    urbi_sched_strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* 5. urbi_strand_unwind_status returns UEXEC_OK for a fresh strand. */
UTEST(capi_strand_unwind_status_ok_on_clean_strand)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);

    UASSERT_EQ(urbi_strand_unwind_status(&vm, &s), UEXEC_OK);
    UASSERT(!urbi_strand_is_fatal(&vm, &s, NULL, NULL));

    free(reg);
    urbi_sched_strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* 6. urbi_throw deposits THROW; urbi_return_val deposits RETURN. */
UTEST(capi_host_callback_helpers_throw_and_return)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);

    /* Simulate urbi_throw from a host callback. */
    urbi_throw(&vm, &s, make_int(42));
    UASSERT_EQ(urbi_strand_unwind_status(&vm, &s), UEXEC_THROW);
    UASSERT_EQ(s.unwind_value.v.i, (int64_t)42);

    /* Clear and simulate urbi_return_val. */
    s.pending_unwind = UEXEC_OK;
    urbi_return_val(&vm, &s, make_int(99));
    UASSERT_EQ(urbi_strand_unwind_status(&vm, &s), UEXEC_RETURN);
    UASSERT_EQ(s.unwind_value.v.i, (int64_t)99);

    free(reg);
    urbi_sched_strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* 7. urbi_tag_stop_local deposits TAG_STOP with the correct target pointer. */
UTEST(capi_tag_stop_local_deposits_tag_stop)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);

    /* Use a non-NULL sentinel as the tag pointer (UTag not defined at M3). */
    struct UTag *fake_tag = (struct UTag *)(void *)0xdeadbeef;
    urbi_tag_stop_local(&vm, &s, fake_tag, make_int(7));

    UASSERT_EQ(urbi_strand_unwind_status(&vm, &s), UEXEC_TAG_STOP);
    UASSERT(s.unwind_target == fake_tag);
    UASSERT_EQ(s.unwind_value.v.i, (int64_t)7);

    free(reg);
    urbi_sched_strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* 8. urbi_tag_stop: valid args return URBI_OK; NULL args return error.
 *    Uses a real (empty) UTag so the member_strands walk is safe. */
UTEST(capi_tag_stop_validates_args)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* NULL vm: must return URBI_ERR_INVALID_ARG. */
    /* Use a stack-allocated real UTag so dereferencing is safe. */
    struct UTag real_tag;
    real_tag.type_tag             = 5U; /* UTYPE_TAG */
    real_tag.gc_byte              = 0;
    real_tag.pad0                 = 0;
    real_tag.flags                = 0;
    real_tag.pad1[0]              = 0;
    real_tag.pad1[1]              = 0;
    real_tag.pad1[2]              = 0;
    real_tag.member_strands_head  = NULL;
    real_tag.member_watchers_head = NULL;
    real_tag.name.kind            = UVAL_NIL;
    real_tag.name.v.i             = 0;

    int rc = urbi_tag_stop(NULL, &real_tag, make_nil());
    UASSERT_EQ(rc, URBI_ERR_INVALID_ARG);

    /* NULL tag: must return URBI_ERR_INVALID_ARG. */
    rc = urbi_tag_stop(&vm, NULL, make_nil());
    UASSERT_EQ(rc, URBI_ERR_INVALID_ARG);

    /* Both valid, empty member list: returns URBI_OK (no strands to deposit). */
    rc = urbi_tag_stop(&vm, &real_tag, make_nil());
    UASSERT_EQ(rc, URBI_OK);

    urbi_vm_destroy(&vm);
}

/* 9. urbi_strand_cancel on a WAITING strand transitions state to READY.
 *    Covers the USTRAND_IS_WAITING branch in urbi_strand_cancel (uunwind.c:336). */
UTEST(capi_strand_cancel_unblocks_waiting_strand)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    UValue *reg = strand_minimal(&s, &vm);

    /* Place the strand in a WAITING state (sleeping) through the real
     * parking transition.  v0.13.3 (SCHED-13): a raw WAITING state stamp
     * would bypass urbi_sched_strand_block's strand_waiting_count increment, so
     * the cancel wake path's decrement would trip the no-saturation
     * assert.  Park properly: RUNNING -> block (the RUNNING-decrement is
     * satisfied by seeding the runnable count). */
    s.state = USTRAND_STATE_RUNNING;
    vm.strand_runnable_count = 1;
    urbi_sched_strand_block(&s, USTRAND_REASON_SLEEP, 1000U);

    int rc = urbi_strand_cancel(&vm, &s, make_nil());
    UASSERT_EQ(rc, URBI_OK);

    /* Cancel must have deposited CANCEL. */
    UASSERT_EQ(urbi_strand_unwind_status(&vm, &s), UEXEC_CANCEL);

    /* A waiting strand must be transitioned to READY so the scheduler can
     * dispatch it and run the unwind walker. */
    UASSERT_EQ((int)USTRAND_GET_STATE(&s), (int)USTRAND_READY);

    free(reg);
    urbi_sched_strand_cleanup_stack_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* 10a (T28 / FOUND-013): urbi_tag_stop runs cleanly when an ISR-check fn is
 *      registered that returns false (i.e. NOT in ISR context).  Pins the
 *      ABI contract documented at urbi_tag_stop in <urbi/urbi.h>: the
 *      URBI_ASSERT_NOT_ISR guard only fires when the registered predicate
 *      returns true, so a host that wires the check fn but is not currently
 *      in ISR sees no behavioural change. */
static bool isr_check_always_false(void *ud) { (void)ud; return false; }

UTEST(capi_tag_stop_passes_isr_guard_when_not_in_isr)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Register an ISR-check predicate that says "no, not in ISR".  The
     * URBI_ASSERT_NOT_ISR macro inside urbi_tag_stop must observe the
     * negative result and proceed without tripping the panic. */
    urbi_set_isr_check_fn(&vm, isr_check_always_false, NULL);

    struct UTag real_tag;
    real_tag.type_tag             = 5U; /* UTYPE_TAG */
    real_tag.gc_byte              = 0;
    real_tag.pad0                 = 0;
    real_tag.flags                = 0;
    real_tag.pad1[0]              = 0;
    real_tag.pad1[1]              = 0;
    real_tag.pad1[2]              = 0;
    real_tag.member_strands_head  = NULL;
    real_tag.member_watchers_head = NULL;
    real_tag.name.kind            = UVAL_NIL;
    real_tag.name.v.i             = 0;

    int rc = urbi_tag_stop(&vm, &real_tag, make_nil());
    UASSERT_EQ(rc, URBI_OK);

    urbi_vm_destroy(&vm);
}

/* 10. urbi_sched_strand_cleanup_stack_init returns -1 when the allocator returns NULL.
 *     Covers the allocation-failure path in ucleanup.c (lines 66-70). */
static void *
null_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ptr; (void)nbytes; (void)ud;
    return NULL; /* always fail */
}

UTEST(capi_cleanup_stack_init_fails_on_null_alloc)
{
    UVM vm;
    UStrand s;

    /* Wire up a failing allocator. */
    urbi_vm_init(&vm, null_alloc, NULL);

    memset(&s, 0, sizeof(s));
    s.vm = &vm;

    int rc = urbi_sched_strand_cleanup_stack_init(&s, &vm, 16);
    UASSERT_EQ(rc, -1);

    /* All cleanup fields must be zero on failure. */
    UASSERT(s.cleanup_base  == NULL);
    UASSERT_EQ((unsigned)s.cleanup_cap,   0U);
    UASSERT_EQ((unsigned)s.cleanup_depth, 0U);
    UASSERT(s.cleanup_top   == NULL);

    urbi_vm_destroy(&vm);
}

/* 11. W2 v0.10.9-tag-state D3 ratify: urbi_tag_stop_local deposited on a
 *     strand that has no TAG_SCOPE for the target tag in its cleanup stack
 *     escalates to fatal via the unwind walker (uunwind.c:304
 *     "empty cleanup stack → fatal escalation").  Closes design-risks
 *     v0.10.7-D.  This unit test mirrors what tag_stop_native triggers when
 *     t.member_strands_head == NULL && !strand_has_tag_in_scope(cur, t). */
UTEST(capi_tag_stop_no_target_escalates_to_fatal)
{
    UVM vm;
    UStrand s;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    UTag *t = urbi_tag_create(&vm, realm, "orphan", 6);
    UASSERT(t != NULL);

    UValue *reg = strand_minimal(&s, &vm);

    /* Pre: clean strand, empty cleanup stack, no TAG_SCOPE entry for t. */
    UASSERT_EQ(urbi_strand_unwind_status(&vm, &s), UEXEC_OK);
    UASSERT_EQ((unsigned)s.cleanup_depth, 0U);

    /* Deposit TAG_STOP locally — what tag_stop_native does for the D3
     * outside-scope case (member_strands_head empty + no scope). */
    urbi_tag_stop_local(&vm, &s, t, make_nil());
    UASSERT_EQ(urbi_strand_unwind_status(&vm, &s), UEXEC_TAG_STOP);
    UASSERT(s.unwind_target == t);

    /* Run the walker.  With an empty cleanup stack and pending TAG_STOP,
     * the walker MUST escalate to fatal at uunwind.c:304. */
    urbi_unwind(&s);

    UASSERT_EQ((unsigned)s.fatal_status, (unsigned)UEXEC_TAG_STOP);
    UASSERT_EQ((unsigned)s.state, (unsigned)USTRAND_STATE_DEAD);

    free(reg);
    urbi_sched_strand_cleanup_stack_destroy(&s, &vm);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void
test_capi_unwind_suite(void)
{
    printf("test_capi_unwind\n");
    utest_run("capi_strand_cancel_deposits_unwind",
              capi_strand_cancel_deposits_unwind);
    utest_run("capi_strand_cancel_rejects_dead_strand",
              capi_strand_cancel_rejects_dead_strand);
    utest_run("capi_strand_panic_marks_fatal",
              capi_strand_panic_marks_fatal);
    utest_run("capi_strand_reset_clears_fatal_and_returns_dormant",
              capi_strand_reset_clears_fatal_and_returns_dormant);
    utest_run("capi_strand_unwind_status_ok_on_clean_strand",
              capi_strand_unwind_status_ok_on_clean_strand);
    utest_run("capi_host_callback_helpers_throw_and_return",
              capi_host_callback_helpers_throw_and_return);
    utest_run("capi_tag_stop_local_deposits_tag_stop",
              capi_tag_stop_local_deposits_tag_stop);
    utest_run("capi_tag_stop_validates_args",
              capi_tag_stop_validates_args);
    utest_run("capi_tag_stop_passes_isr_guard_when_not_in_isr",
              capi_tag_stop_passes_isr_guard_when_not_in_isr);
    utest_run("capi_strand_cancel_unblocks_waiting_strand",
              capi_strand_cancel_unblocks_waiting_strand);
    utest_run("capi_cleanup_stack_init_fails_on_null_alloc",
              capi_cleanup_stack_init_fails_on_null_alloc);
    utest_run("capi_tag_stop_no_target_escalates_to_fatal",
              capi_tag_stop_no_target_escalates_to_fatal);
}
