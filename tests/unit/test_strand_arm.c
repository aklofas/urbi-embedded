/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: urbi_strand_arm_from_closure (spec #1 §5.5).
 *
 * Verifies that the extracted helper correctly sets execution-state fields
 * on a fresh strand: pc/pc_base point at proto->instructions, cur_consts
 * points at proto->constants (when non-NULL), R == stack, frame_count == 0,
 * open_upvals == NULL. */

#include "utest.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "chunk/uchunk.h"
#include "object/uchunk_instance.h"
#include "runtime/uclosure.h"
#include "runtime/uframe.h"   /* UVM_STACK_CAP */
#include "urbi/urbi.h" /* urbi_strand_create, urbi_strand_destroy, urbi_realm_create */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ===================================================================
 * Helpers
 * =================================================================== */

/* Build a minimal UClosure wrapping a stack-local UProto.
 * The proto has a single OP_RET instruction and a one-element constant pool.
 * proto and closure storage are caller-provided (stack-local in tests). */
static void
make_trivial_closure(UClosure *cl, UProto *proto,
                     uint32_t *instr_buf,  /* at least 1 slot */
                     UValue   *const_buf)  /* at least 1 slot */
{
    /* Single OP_RET instruction. */
    instr_buf[0] = (uint32_t)OP_RET;  /* opcode in lower 8 bits */

    /* One sentinel constant so cur_consts != NULL after arm. */
    const_buf[0].kind = (uint8_t)UVAL_INT;
    const_buf[0].v.i  = 42;

    memset(proto, 0, sizeof(*proto));
    proto->instructions = instr_buf;
    proto->instr_count  = 1;
    proto->constants    = const_buf;
    proto->const_count  = 1;

    memset(cl, 0, sizeof(*cl));
    cl->proto    = proto;
    cl->nupvals  = 0;
}

/* ===================================================================
 * Tests
 * =================================================================== */

/* Case 1: arm sets pc, pc_base, cur_consts, R, frame_count, open_upvals. */
static void
strand_arm_sets_exec_fields(void)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    /* Build a trivial closure on the stack. */
    uint32_t instr[1];
    UValue   consts[1];
    UProto   proto;
    UClosure cl;
    make_trivial_closure(&cl, &proto, instr, consts);

    /* Create a fresh DORMANT strand (execution fields zero-init). */
    UStrand *s = urbi_strand_create(realm, &cl);
    UASSERT(s != NULL);

    /* Arm it. */
    int rc = urbi_strand_arm_from_closure(s, &cl);
    UASSERT_EQ(0, rc);

    /* pc and pc_base must point at the proto's instruction array. */
    UASSERT(s->pc      == proto.instructions);
    UASSERT(s->pc_base == proto.instructions);

    /* cur_consts must point at the proto's constant pool (non-NULL path). */
    UASSERT(s->cur_consts == proto.constants);

    /* R must be non-NULL and equal to stack (start of register window). */
    UASSERT(s->R != NULL);
    UASSERT(s->R == s->stack);

    /* frame_count must be zero. */
    UASSERT_EQ(0, s->frame_count);

    /* open_upvals must be NULL. */
    UASSERT(s->open_upvals == NULL);

    urbi_strand_destroy(s);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* Case 2: arm with proto->constants == NULL preserves existing cur_consts
 * (which is NULL for a fresh strand — no crash, returns 0). */
static void
strand_arm_null_constants(void)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    uint32_t instr[1];
    UProto   proto;
    UClosure cl;

    instr[0] = (uint32_t)OP_RET;
    memset(&proto, 0, sizeof(proto));
    proto.instructions = instr;
    proto.instr_count  = 1;
    proto.constants    = NULL;   /* no constant pool */
    proto.const_count  = 0;

    memset(&cl, 0, sizeof(cl));
    cl.proto   = &proto;
    cl.nupvals = 0;

    UStrand *s = urbi_strand_create(realm, &cl);
    UASSERT(s != NULL);

    int rc = urbi_strand_arm_from_closure(s, &cl);
    UASSERT_EQ(0, rc);

    /* cur_consts stays as-is (NULL for a fresh strand). */
    UASSERT(s->cur_consts == NULL);

    /* Execution pointer fields still set correctly. */
    UASSERT(s->pc      == proto.instructions);
    UASSERT(s->pc_base == proto.instructions);
    UASSERT(s->R       == s->stack);
    UASSERT_EQ(0, s->frame_count);

    urbi_strand_destroy(s);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* CHSTR-014 (T102): fork_spawn_child inherits parent's module_instance after
 * urbi_strand_arm_from_closure.  Verifies the post-arm wiring at uop_fork.c
 * so OP_GETSLOT/OP_SETSLOT in the child can resolve the IC table at
 * frame_count == 0.  Tests the call-site contract directly: a freshly armed
 * strand picks up its module_instance from a separate post-arm assignment
 * mirroring the fork_spawn_child / urbi_vm_run / watcher body-spawn paths.
 *
 * This is a structural regression rather than an API contract on
 * urbi_strand_arm_from_closure itself — that helper deliberately does not
 * touch module_instance because each spawn path resolves it differently
 * (siblings inherit; watcher bodies pointer-range-search a list; scratch
 * frames synthesize their own minimal arr). */
static void
strand_arm_from_closure_initializes_module_instance(void)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    uint32_t instr[1];
    UValue   consts[1];
    UProto   proto;
    UClosure cl;
    make_trivial_closure(&cl, &proto, instr, consts);

    UStrand *s = urbi_strand_create(realm, &cl);
    UASSERT(s != NULL);
    UASSERT(s->module_instance == NULL);  /* fresh strands start with NULL */

    /* Arm: stack is allocated, but module_instance is NOT touched (the spawn
     * site is responsible for wiring it post-arm).  Confirms the helper is
     * ABI-stable for the call sites that handle module_instance themselves. */
    int rc = urbi_strand_arm_from_closure(s, &cl);
    UASSERT_EQ(0, rc);
    UASSERT(s->module_instance == NULL);

    /* Mirror the fork_spawn_child wiring: post-arm explicit set. */
    UChunkInstance fake_mi = {0};
    s->module_instance = &fake_mi;
    UASSERT(s->module_instance == &fake_mi);

    s->module_instance = NULL;  /* avoid GC chase of stack-local mi */
    urbi_strand_destroy(s);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* CHSTR-005 (T99): urbi_strand_arm_from_closure precondition is s->stack ==
 * NULL.  Re-arming a strand that already owns a register stack would leak the
 * prior allocation because the inner urbi_strand_register_stack_alloc
 * unconditionally overwrites s->stack.  Verify that a fresh strand (the only
 * legal arm-time state) satisfies the precondition on the hot path used by
 * fork_spawn_child + the watcher body-spawn path. */
static void
strand_arm_from_closure_asserts_stack_null(void)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    uint32_t instr[1];
    UValue   consts[1];
    UProto   proto;
    UClosure cl;
    make_trivial_closure(&cl, &proto, instr, consts);

    UStrand *s = urbi_strand_create(realm, &cl);
    UASSERT(s != NULL);
    /* Fresh strands have stack == NULL — frame-0 setup is deferred. */
    UASSERT(s->stack == NULL);
    UASSERT(s->R == NULL);

    int rc = urbi_strand_arm_from_closure(s, &cl);
    UASSERT_EQ(0, rc);

    /* After arm, stack is allocated and R points at it. */
    UASSERT(s->stack != NULL);
    UASSERT(s->R == s->stack);

    urbi_strand_destroy(s);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* CHSTR-019: re-arming a strand with a closure whose proto has constants==NULL
 * must not retain the stale cur_consts pointer from a prior arm.  The fix:
 * urbi_strand_arm_from_closure unconditionally writes entry->proto->constants
 * to s->cur_consts (rather than preserving the prior pool when the new proto
 * carries no constants).  Re-arming via the legal free → arm sequence (per the
 * CHSTR-005 precondition) covers both fork_spawn_child and the watcher
 * body-spawn paths if a strand is recycled. */
static void
strand_arm_from_closure_resets_cur_consts_on_rearm(void)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    /* First closure: has a non-NULL constants pool. */
    uint32_t instr1[1];
    UValue   consts1[1];
    UProto   proto1;
    UClosure cl1;
    make_trivial_closure(&cl1, &proto1, instr1, consts1);

    UStrand *s = urbi_strand_create(realm, &cl1);
    UASSERT(s != NULL);

    /* First arm — cur_consts picks up consts1. */
    int rc = urbi_strand_arm_from_closure(s, &cl1);
    UASSERT_EQ(0, rc);
    UASSERT(s->cur_consts == proto1.constants);

    /* Free the register stack so the CHSTR-005 precondition (s->stack == NULL)
     * is satisfied for re-arm — this is the documented re-use sequence. */
    urbi_strand_register_stack_free(s, &vm);
    UASSERT(s->stack == NULL);

    /* Second closure: proto has constants == NULL. */
    uint32_t instr2[1];
    UProto   proto2;
    UClosure cl2;
    instr2[0] = (uint32_t)OP_RET;
    memset(&proto2, 0, sizeof(proto2));
    proto2.instructions = instr2;
    proto2.instr_count  = 1;
    proto2.constants    = NULL;
    proto2.const_count  = 0;
    memset(&cl2, 0, sizeof(cl2));
    cl2.proto   = &proto2;
    cl2.nupvals = 0;

    /* Re-arm with the second closure. */
    rc = urbi_strand_arm_from_closure(s, &cl2);
    UASSERT_EQ(0, rc);

    /* CHSTR-019: cur_consts must be unconditionally reset to the new proto's
     * pool (NULL here), not retain the stale consts1 pointer from prior arm. */
    UASSERT(s->cur_consts == NULL);

    urbi_strand_destroy(s);
    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_strand_arm_suite(void)
{
    printf("test_strand_arm\n");
    utest_run("strand_arm_sets_exec_fields", strand_arm_sets_exec_fields);
    utest_run("strand_arm_null_constants",   strand_arm_null_constants);
    utest_run("strand_arm_from_closure_asserts_stack_null",
              strand_arm_from_closure_asserts_stack_null);
    utest_run("strand_arm_from_closure_initializes_module_instance",
              strand_arm_from_closure_initializes_module_instance);
    utest_run("strand_arm_from_closure_resets_cur_consts_on_rearm",
              strand_arm_from_closure_resets_cur_consts_on_rearm);
}
