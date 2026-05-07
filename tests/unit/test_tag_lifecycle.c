/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: UTag create/destroy + urbi_strand_scope_tag (T29, row 11).
 * Extended at T30: OP_PUSH_TAG/POP_TAG member-list bookkeeping. */

#include "utest.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "runtime/ucleanup.h"
#include "tag/utag.h"
#include "gc/ugc.h"    /* UTYPE_TAG */
#include "urbi/urbi.h"
#include "module/umodule.h"   /* UValue, UValKind, UVM_STACK_CAP */
#include "sched/usched_cooperative.h"

#include <stdlib.h>

#define UTEST(name) static void name(void)

/* === Helpers === */

/* Counting allocator: counts allocation calls (not frees).
   Fails with NULL when alloc_calls > fail_at (fail_at == -1 means never). */
typedef struct {
    int alloc_calls;
    int fail_at;
} AllocSpy;

static void *
spy_alloc(void *ptr, size_t n, void *ud)
{
    AllocSpy *spy = (AllocSpy *)ud;
    if (n > 0 && ptr == NULL) {
        spy->alloc_calls++;
        if (spy->fail_at >= 0 && spy->alloc_calls > spy->fail_at)
            return NULL;
    }
    if (n == 0) { free(ptr); return NULL; }
    return realloc(ptr, n);
}

/* === Test cases === */

/* 1. utag_create_basic: realm->tag is non-NULL with correct fields. */
UTEST(utag_create_basic)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    /* T29: realm->tag is now a real UTag, not NULL. */
    UASSERT(r->tag != NULL);
    UASSERT_EQ((unsigned)r->tag->type_tag, (unsigned)UTYPE_TAG);
    UASSERT_EQ((unsigned)r->tag->gc_byte,  0U);
    UASSERT_EQ((unsigned)r->tag->flags,    0U);
    UASSERT(r->tag->member_strands_head  == NULL);
    UASSERT(r->tag->member_watchers_head == NULL);
    UASSERT_EQ((unsigned)r->tag->name.kind, (unsigned)UVAL_NIL);

    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 2. utag_create_oom: allocator fails on the tag alloc → realm_create returns NULL.
 *
 * urbi_realm_create allocation order:
 *   call 1: realm struct
 *   call 2: UTag (utag_create)
 *   call 3: UNamespace (unamespace_create — includes internal alloc(s))
 *
 * We fail at call 2 (fail_at == 1, so call #2 returns NULL) to exercise the
 * OOM rollback path that frees the realm struct and returns NULL. */
UTEST(utag_create_oom)
{
    UVM vm;
    URealm *r;

    /* Calibration run: count how many allocs a successful realm_create uses. */
    AllocSpy spy1 = { 0, -1 };
    uvm_init(&vm, spy_alloc, &spy1);
    r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);

    /* The realm struct is alloc call #1, UTag is #2.
     * Fail at call index 2 (fail_at == 1 means > 1 fails, i.e. call #2 onward). */
    AllocSpy spy2 = { 0, 1 };   /* fail on alloc_calls > 1 = fail on call #2+ */
    uvm_init(&vm, spy_alloc, &spy2);
    r = urbi_realm_create(&vm);
    UASSERT(r == NULL);           /* OOM: UTag alloc failed → whole create returns NULL */
    UASSERT(vm.realms_head == NULL);  /* no partial realm was linked */
    uvm_destroy(&vm);
}

/* 3. utag_destroy_null_safe: utag_destroy(vm, NULL) is a no-op. */
UTEST(utag_destroy_null_safe)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    /* Must not crash. */
    utag_destroy(&vm, NULL);
    uvm_destroy(&vm);
}

/* 4. strand_scope_tag_returns_innermost: scope_tag walks top-down and returns
 *    the owning_tag of the innermost TAG_SCOPE entry. */
UTEST(strand_scope_tag_returns_innermost)
{
    UVM vm;
    UTag inner_tag;
    UCleanupEntry *e;

    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UASSERT(r->tag != NULL);

    /* Create a strand — its cleanup-stack starts with [realm->tag synthetic]. */
    UStrand *s = urbi_strand_create(r, NULL);
    UASSERT(s != NULL);
    UASSERT(s->cleanup_depth == 1);  /* one ambient entry: realm->tag */

    /* The current innermost scope tag is realm->tag. */
    UASSERT(urbi_strand_scope_tag(s) == r->tag);

    /* Push an inner tag scope manually (simulating OP_PUSH_TAG).
     * inner_tag is stack-allocated for simplicity — just need its address. */
    inner_tag.type_tag             = UTYPE_TAG;
    inner_tag.gc_byte              = 0;
    inner_tag.pad0                 = 0;
    inner_tag.flags                = 0;
    inner_tag.pad1[0]              = 0;
    inner_tag.pad1[1]              = 0;
    inner_tag.pad1[2]              = 0;
    inner_tag.member_strands_head  = NULL;
    inner_tag.member_watchers_head = NULL;
    inner_tag.enter_event          = NULL;
    inner_tag.leave_event          = NULL;
    inner_tag.name.kind            = UVAL_NIL;
    inner_tag.name.v.i             = 0;

    e = strand_cleanup_push(s);
    UASSERT(e != NULL);
    e->kind        = (uint8_t)UCLEANUP_TAG_SCOPE;
    e->flags       = 0;
    e->owning_tag  = &inner_tag;
    e->strand_back = s;
    e->next_member = inner_tag.member_strands_head;
    inner_tag.member_strands_head = e;

    /* Now scope_tag must return inner_tag (topmost TAG_SCOPE). */
    UASSERT(urbi_strand_scope_tag(s) == &inner_tag);

    /* Unlink inner_tag entry before destroy (maintain invariant for utag_destroy
     * assertion; inner_tag is stack-allocated so utag_destroy isn't called on it,
     * but ustrand_destroy will unlink all TAG_SCOPE entries automatically). */
    urbi_strand_destroy(s);
    /* inner_tag.member_strands_head should be NULL after strand_unlink_from_tags. */
    UASSERT(inner_tag.member_strands_head == NULL);

    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 5. strand_scope_tag_empty_returns_null: a strand with cleanup_depth == 0
 *    returns NULL from scope_tag. */
UTEST(strand_scope_tag_empty_returns_null)
{
    UVM vm;
    UStrand s;

    uvm_init(&vm, NULL, NULL);

    /* Use ustrand_init directly — bypasses ambient-tag attachment, depth == 0. */
    ustrand_init(&s, &vm);
    UASSERT(s.cleanup_depth == 0);

    UASSERT(urbi_strand_scope_tag(&s) == NULL);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* 6. strand_scope_tag_null_safe: NULL strand returns NULL. */
UTEST(strand_scope_tag_null_safe)
{
    UASSERT(urbi_strand_scope_tag(NULL) == NULL);
}

/* 7. utag_type_tag_constant: UTYPE_TAG has value 5. */
UTEST(utag_type_tag_constant)
{
    UASSERT_EQ((unsigned)UTYPE_TAG, 5U);
}

/* ============================================================
 * T30: OP_PUSH_TAG / OP_POP_TAG member-list bookkeeping tests.
 *
 * These tests use the same dispatch_loop pattern as test_dispatch_loop.c:
 * a synthetic UStrand with a manually allocated cleanup stack is driven
 * through dispatch_loop_until_yield to exercise the opcode handlers.
 * ============================================================ */

/* Bytecode encoding helpers (mirrors test_dispatch_loop.c local helpers). */
static uint32_t t30_enc_push_tag(uint8_t flags_nibble, uint8_t tag_reg, uint16_t onleave_pc) {
    uint8_t a = (uint8_t)((flags_nibble << 4) | (tag_reg & 0x0FU));
    return uinstr_enc_abx(OP_PUSH_TAG, a, onleave_pc);
}
static uint32_t t30_enc_pop_tag(uint8_t tag_reg) {
    return uinstr_enc_abc(OP_POP_TAG, tag_reg, 0, 0);
}
static uint32_t t30_enc_loadnil(uint8_t dst) {
    return uinstr_enc_abc(OP_LOADNIL, dst, 0, 0);
}
static uint32_t t30_enc_ret(void) {
    return uinstr_enc_abc(OP_RET, 0, 0, 0);
}

/* strand_setup_t30: zero-initialize a UStrand and wire up the minimum fields
 * required by dispatch_loop_until_yield.  Mirrors the helper in
 * test_dispatch_loop.c; duplicated here to keep the test self-contained. */
static void
strand_setup_t30(UStrand *s, UVM *vm,
                 const uint32_t *instructions,
                 UValue *reg_stack)
{
    volatile unsigned char *p = (volatile unsigned char *)s;
    size_t n = sizeof(*s);
    size_t i;
    for (i = 0; i < n; i++) p[i] = 0;

    s->vm         = vm;
    s->state      = USTRAND_STATE_RUNNING;
    s->stack      = reg_stack;
    s->R          = reg_stack;
    s->pc         = instructions;
    s->pc_base    = instructions;
    s->cur_consts = NULL;
    s->module     = NULL;
    s->frame_count  = 0;
    s->open_upvals  = NULL;
    s->closure_list = NULL;
    s->closed_cells = NULL;
    s->out_slot     = NULL;
}

/* strand_setup_cleanup_t30: allocate a fresh cleanup stack for the strand. */
static void
strand_setup_cleanup_t30(UStrand *s)
{
    s->cleanup_base  = (UCleanupEntry *)calloc(64, sizeof(UCleanupEntry));
    s->cleanup_cap   = 64;
    s->cleanup_depth = 0;
    s->cleanup_top   = NULL;
}

/* 8. op_push_tag_inserts_member_strands_and_pop_clears:
 *    Run PUSH_TAG; LOADNIL; POP_TAG; RET through dispatch_loop.
 *    After POP_TAG the cleanup_depth must be 0.
 *    ASan/UBSan will catch any UTag leak or double-free in POP_TAG. */
UTEST(op_push_tag_inserts_member_strands_and_pop_clears)
{
    static uint32_t instrs[4];
    instrs[0] = t30_enc_push_tag(0, 0, 0U);
    instrs[1] = t30_enc_loadnil(1);
    instrs[2] = t30_enc_pop_tag(0);
    instrs[3] = t30_enc_ret();

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    UStrand s;
    strand_setup_t30(&s, &vm, instrs, reg_stack);
    strand_setup_cleanup_t30(&s);

    UValue retval = {0};
    s.out_slot = &retval;

    uint64_t consumed = dispatch_loop_until_yield(&s, 10000U);

    /* Strand must reach DEAD (top-level RET). */
    UASSERT_EQ((int)USTRAND_STATE_DEAD, (int)s.state);
    UASSERT(consumed >= 1U);
    /* Cleanup stack must be empty after POP_TAG. */
    UASSERT_EQ((int)s.cleanup_depth, 0);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* 9. op_push_tag_member_strands_head_wired:
 *    Inspect the cleanup entry after PUSH_TAG but before POP_TAG.
 *    We dispatch only the PUSH_TAG instruction (step_budget=1 or force
 *    a stop via a short instruction sequence), then check the entry.
 *    Strategy: PUSH_TAG; RET — dispatch to DEAD in one pass; then verify
 *    the owning_tag of the cleanup entry that was pushed (it gets popped
 *    by OP_RET unwinding? — no: RET at top-level goes DEAD without cleanup pop).
 *    A cleaner approach: push manually via the strand API, then verify. */
UTEST(op_push_tag_member_strands_head_wired)
{
    /* Dispatch PUSH_TAG then stop just after by placing PUSH_TAG at pc=0
     * and a deliberate YIELD at pc=1 so the strand pauses with the tag scope open.
     * After the pause, inspect the cleanup entry. */
    static uint32_t instrs[3];
    instrs[0] = t30_enc_push_tag(0, 0, 0U);
    instrs[1] = uinstr_enc_abc(OP_YIELD, 0, 0, 0);  /* pause here */
    instrs[2] = t30_enc_ret();

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    UStrand s;
    strand_setup_t30(&s, &vm, instrs, reg_stack);
    strand_setup_cleanup_t30(&s);

    /* Dispatch: PUSH_TAG (executes) + YIELD (pauses). */
    dispatch_loop_until_yield(&s, 10000U);

    /* Strand should be READY (yielded at OP_YIELD). */
    UASSERT_EQ((int)USTRAND_STATE_READY, (int)s.state);
    /* cleanup_depth must be 1 (the PUSH_TAG entry). */
    UASSERT_EQ((int)s.cleanup_depth, 1);

    /* Inspect the top entry. */
    UCleanupEntry *top = &s.cleanup_base[0];
    UASSERT_EQ((unsigned)top->kind, (unsigned)UCLEANUP_TAG_SCOPE);
    /* owning_tag must be non-NULL (T30 allocates per-scope UTag). */
    UASSERT(top->owning_tag != NULL);
    /* member_strands_head on the tag must point back to this entry. */
    UASSERT(top->owning_tag->member_strands_head == top);
    /* strand_back must be wired. */
    UASSERT(top->strand_back == &s);
    /* next_member must be NULL (only member of this fresh tag). */
    UASSERT(top->next_member == NULL);

    /* Drain ready queue manually before destroying. */
    if (vm.ready_head == &s) {
        vm.ready_head = s.ready_next;
        if (vm.ready_head == NULL) vm.ready_tail = NULL;
        if (vm.strand_runnable_count > 0) vm.strand_runnable_count--;
    }

    /* Manually destroy the owning_tag (since we won't execute POP_TAG —
     * the strand will be abandoned). Unlink from member list first to
     * satisfy utag_destroy's §3.5 assertion. */
    UTag *tag = top->owning_tag;
    tag->member_strands_head = NULL;  /* manual unlink */
    utag_destroy(&vm, tag);
    top->owning_tag = NULL;

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* 10. op_push_tag_oom_marks_strand_fatal:
 *     Install a spy allocator that fails on all allocations (fail_at = 0).
 *     uvm_init's event_ring alloc fails first and tolerates NULL gracefully;
 *     utag_create's subsequent allocation failure triggers the OP_PUSH_TAG
 *     fatal path under test.  Verify the strand goes fatal with UEXEC_THROW
 *     and state DEAD. */
UTEST(op_push_tag_oom_marks_strand_fatal)
{
    static uint32_t instrs[2];
    instrs[0] = t30_enc_push_tag(0, 0, 0U);
    instrs[1] = t30_enc_ret();

    UVM vm;
    /* fail_at == 0: spy_alloc fails on alloc_calls > 0, i.e. the very first
     * allocation (alloc_calls becomes 1 on call #1, which is > 0 → NULL). */
    AllocSpy spy = { 0, 0 };
    uvm_init(&vm, spy_alloc, &spy);
    sched_init(&vm, NULL);

    /* The cleanup stack is allocated via calloc() directly (not vm->alloc_fn),
     * so spy_alloc won't intercept it. */
    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    UStrand s;
    strand_setup_t30(&s, &vm, instrs, reg_stack);
    strand_setup_cleanup_t30(&s);

    dispatch_loop_until_yield(&s, 10000U);

    /* OOM during utag_create → fatal_status=UEXEC_THROW, state=DEAD. */
    UASSERT_EQ((int)s.state,        (int)USTRAND_STATE_DEAD);
    UASSERT_EQ((int)s.fatal_status, (int)UEXEC_THROW);
    /* cleanup_depth must be 0 — entry was never pushed. */
    UASSERT_EQ((int)s.cleanup_depth, 0);

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* 11. op_push_tag_cleanup_overflow_releases_tag:
 *     Fill the cleanup stack to URBI_CLEANUP_MAX, then dispatch PUSH_TAG.
 *     The strand_cleanup_push call fails; the rollback utag_destroy must
 *     free the tag — otherwise ASan reports a leak. */
UTEST(op_push_tag_cleanup_overflow_releases_tag)
{
    static uint32_t instrs[2];
    instrs[0] = t30_enc_push_tag(0, 0, 0U);
    instrs[1] = t30_enc_ret();

    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UValue *reg_stack = (UValue *)calloc(UVM_STACK_CAP, sizeof(UValue));
    UASSERT(reg_stack != NULL);

    UStrand s;
    strand_setup_t30(&s, &vm, instrs, reg_stack);
    strand_setup_cleanup_t30(&s);

    /* Fill the cleanup stack to capacity so strand_cleanup_push returns NULL. */
    s.cleanup_depth = s.cleanup_cap;

    dispatch_loop_until_yield(&s, 10000U);

    /* cleanup_push failure → utag_destroy rollback → fatal, DEAD. */
    UASSERT_EQ((int)s.state,        (int)USTRAND_STATE_DEAD);
    UASSERT_EQ((int)s.fatal_status, (int)UEXEC_THROW);

    /* Reset cleanup_depth to 0 before freeing (was artificially set to cap). */
    s.cleanup_depth = 0;

    ustrand_destroy(&s, &vm);
    uvm_destroy(&vm);
}

/* ============================================================
 * §9.1 gap-fill cases: nested membership + realm root
 * ============================================================ */

/* Helper: init a stack-allocated UTag for use in tests that don't
 * need the GC to manage the allocation. */
static void
tag_init_local_lifecycle(UTag *t)
{
    t->type_tag             = UTYPE_TAG;
    t->gc_byte              = 0;
    t->pad0                 = 0;
    t->flags                = 0;
    t->pad1[0]              = 0;
    t->pad1[1]              = 0;
    t->pad1[2]              = 0;
    t->member_strands_head  = NULL;
    t->member_watchers_head = NULL;
    t->enter_event          = NULL;
    t->leave_event          = NULL;
    t->name.kind            = UVAL_NIL;
    t->name.v.i             = 0;
}

/* Helper: push a synthetic TAG_SCOPE entry for tag onto s's cleanup stack.
 * Wires next_member + member_strands_head + strand_back. */
static UCleanupEntry *
push_tag_scope_lifecycle(UStrand *s, UTag *tag)
{
    UCleanupEntry *e = strand_cleanup_push(s);
    if (!e) return NULL;
    e->kind        = (uint8_t)UCLEANUP_TAG_SCOPE;
    e->flags       = 0;
    e->owning_tag  = tag;
    e->strand_back = s;
    e->next_member = tag->member_strands_head;
    tag->member_strands_head = e;
    return e;
}

/* Count how many entries in a tag's member_strands_head list point to strand s. */
static int
count_strand_in_tag_members(UTag *tag, UStrand *s)
{
    UCleanupEntry *e = tag->member_strands_head;
    int n = 0;
    while (e != NULL) {
        if (e->strand_back == s) n++;
        e = e->next_member;
    }
    return n;
}

/* 12. nested_tag_membership
 *
 * Push 3 nested tag scopes (tag_a, tag_b, tag_c) onto a strand.
 * Verify the strand appears in each tag's member_strands_head list.
 * Then manually pop one scope at a time (via urbi_strand_destroy to
 * exercise the strand_unlink_from_tags path) by destroying the strand. */
UTEST(nested_tag_membership)
{
    UVM vm;
    UTag tag_a, tag_b, tag_c;

    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    /* Create strand — inherits realm->tag as depth 0. */
    UStrand *s = urbi_strand_create(r, NULL);
    UASSERT(s != NULL);
    UASSERT_EQ((unsigned)s->cleanup_depth, 1U);

    /* Push three more tag scopes. */
    tag_init_local_lifecycle(&tag_a);
    tag_init_local_lifecycle(&tag_b);
    tag_init_local_lifecycle(&tag_c);

    UASSERT(push_tag_scope_lifecycle(s, &tag_a) != NULL);
    UASSERT(push_tag_scope_lifecycle(s, &tag_b) != NULL);
    UASSERT(push_tag_scope_lifecycle(s, &tag_c) != NULL);
    UASSERT_EQ((unsigned)s->cleanup_depth, 4U);

    /* Strand must appear in all four tags' member lists. */
    UASSERT_EQ(count_strand_in_tag_members(r->tag,  s), 1);
    UASSERT_EQ(count_strand_in_tag_members(&tag_a, s), 1);
    UASSERT_EQ(count_strand_in_tag_members(&tag_b, s), 1);
    UASSERT_EQ(count_strand_in_tag_members(&tag_c, s), 1);

    /* Destroy strand: strand_unlink_from_tags must clear all three
     * stack-allocated tags' member_strands_head. */
    urbi_strand_destroy(s);

    UASSERT(tag_a.member_strands_head == NULL);
    UASSERT(tag_b.member_strands_head == NULL);
    UASSERT(tag_c.member_strands_head == NULL);
    /* realm->tag unlinked too. */
    UASSERT(r->tag->member_strands_head == NULL);

    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 13. realm_root_at_bottom
 *
 * Verify that every strand created via urbi_strand_create has
 * realm->tag as the bottommost (index 0) TAG_SCOPE entry. */
UTEST(realm_root_at_bottom)
{
    UVM vm;

    uvm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);
    UASSERT(r->tag != NULL);

    UStrand *s = urbi_strand_create(r, NULL);
    UASSERT(s != NULL);

    /* cleanup_depth must be at least 1. */
    UASSERT(s->cleanup_depth >= 1U);

    /* The bottommost entry (index 0) must be a TAG_SCOPE with realm->tag. */
    UCleanupEntry *bottom = &s->cleanup_base[0];
    UASSERT_EQ((unsigned)bottom->kind, (unsigned)UCLEANUP_TAG_SCOPE);
    UASSERT(bottom->owning_tag == r->tag);
    UASSERT(bottom->strand_back == s);

    /* The strand must appear in realm->tag's member list. */
    UASSERT(count_strand_in_tag_members(r->tag, s) == 1);

    urbi_strand_destroy(s);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* 14. tag_stop_synchronous_no_bytecode
 *
 * After urbi_tag_stop, strands have pending_unwind == TAG_STOP but their
 * state does not change (no bytecode executes synchronously — the deposit
 * only sets the flag; the scheduler drives execution to the unwind walker).
 * Verify: state remains DORMANT (not DEAD), and no instruction counter
 * advances (the strand's pc is unchanged). */
UTEST(tag_stop_synchronous_no_bytecode)
{
    UVM vm;
    UValue nil;

    uvm_init(&vm, NULL, NULL);

    nil.kind = UVAL_NIL;
    nil.v.i  = 0;

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    UStrand *s = urbi_strand_create(r, NULL);
    UASSERT(s != NULL);

    /* Record initial state. */
    const uint32_t *pc_before = s->pc;
    uint8_t         state_before = s->state;

    /* Perform synchronous stop. */
    int rc = urbi_tag_stop(&vm, r->tag, nil);
    UASSERT_EQ(rc, URBI_OK);

    /* pending_unwind set — but state unchanged (no bytecode ran). */
    UASSERT_EQ((int)s->pending_unwind, (int)UEXEC_TAG_STOP);
    UASSERT_EQ((int)s->state, (int)state_before);

    /* pc must not have advanced — urbi_tag_stop runs no bytecode. */
    UASSERT(s->pc == pc_before);

    urbi_strand_destroy(s);
    urbi_realm_destroy(&vm, r);
    uvm_destroy(&vm);
}

/* === Suite entry point === */

void
test_tag_lifecycle_suite(void)
{
    printf("test_tag_lifecycle\n");
    utest_run("utag_create_basic",                 utag_create_basic);
    utest_run("utag_create_oom",                   utag_create_oom);
    utest_run("utag_destroy_null_safe",            utag_destroy_null_safe);
    utest_run("strand_scope_tag_returns_innermost",strand_scope_tag_returns_innermost);
    utest_run("strand_scope_tag_empty_returns_null",strand_scope_tag_empty_returns_null);
    utest_run("strand_scope_tag_null_safe",        strand_scope_tag_null_safe);
    utest_run("utag_type_tag_constant",            utag_type_tag_constant);
    /* T30: OP_PUSH_TAG / OP_POP_TAG member-list bookkeeping */
    utest_run("op_push_tag inserts member_strands and pop clears",
              op_push_tag_inserts_member_strands_and_pop_clears);
    utest_run("op_push_tag member_strands_head wired after push",
              op_push_tag_member_strands_head_wired);
    utest_run("op_push_tag OOM marks strand fatal",
              op_push_tag_oom_marks_strand_fatal);
    utest_run("op_push_tag cleanup overflow releases tag",
              op_push_tag_cleanup_overflow_releases_tag);
    /* §9.1 gap-fill: nested membership + realm-root + synchronous-no-bytecode */
    utest_run("nested_tag_membership",            nested_tag_membership);
    utest_run("realm_root_at_bottom",             realm_root_at_bottom);
    utest_run("tag_stop_synchronous_no_bytecode", tag_stop_synchronous_no_bytecode);
}
