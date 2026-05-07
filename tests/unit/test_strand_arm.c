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
#include "ustrand.h"
#include "umodule.h"
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
    uvm_init(&vm, NULL, NULL);

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
    uvm_destroy(&vm);
}

/* Case 2: arm with proto->constants == NULL preserves existing cur_consts
 * (which is NULL for a fresh strand — no crash, returns 0). */
static void
strand_arm_null_constants(void)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

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
    uvm_destroy(&vm);
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
}
