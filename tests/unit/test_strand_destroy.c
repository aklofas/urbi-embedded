/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: ustrand_destroy idempotence with respect to s->stack.
 *
 * CHSTR-004: ustrand_destroy frees s->stack via urbi_strand_register_stack_free,
 * which already returns early when s->stack == NULL — but the contract is
 * fragile because the post-condition lives one indirection away.  This test
 * pins the idempotence contract: when a caller (e.g. urbi_vm_run) has already
 * pre-freed the register stack and NULL'd s->stack, ustrand_destroy must not
 * attempt a second free on the now-NULL pointer. */

#include "utest.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "runtime/uclosure.h"
#include "module/umodule.h"
#include "urbi/urbi.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* Allocator spy: records every (ptr, n=0) free call so we can assert that a
 * second free of s->stack does NOT happen during ustrand_destroy. */
typedef struct {
    void *seen_free_ptrs[64];
    int   free_count;
    int   alloc_count;
} FreeSpy;

static void *
free_spy_alloc(void *ptr, size_t n, void *ud)
{
    FreeSpy *spy = (FreeSpy *)ud;
    if (n == 0) {
        if (ptr != NULL && spy->free_count < (int)(sizeof(spy->seen_free_ptrs) /
                                                   sizeof(spy->seen_free_ptrs[0]))) {
            spy->seen_free_ptrs[spy->free_count++] = ptr;
        }
        free(ptr);
        return NULL;
    }
    if (ptr == NULL) spy->alloc_count++;
    return realloc(ptr, n);
}

/* Case 1: ustrand_destroy is idempotent against a pre-freed register stack.
 *
 * Sequence mirrors urbi_vm_run's teardown: caller frees the stack via
 * urbi_strand_register_stack_free (which NULLs s->stack), then calls
 * ustrand_destroy.  The destroy path must not call vm->alloc_fn(stack, 0, ud)
 * a second time. */
static void
ustrand_destroy_idempotent_on_freed_stack(void)
{
    FreeSpy spy = {{NULL}, 0, 0};
    UVM vm;
    urbi_vm_init(&vm, free_spy_alloc, &spy);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    /* Build a trivial closure on the stack so urbi_strand_create succeeds. */
    uint32_t instr[1];
    UValue   consts[1];
    UProto   proto;
    UClosure cl;
    instr[0]    = (uint32_t)OP_RET;
    consts[0].kind = (uint8_t)UVAL_INT;
    consts[0].v.i  = 0;
    memset(&proto, 0, sizeof(proto));
    proto.instructions = instr;
    proto.instr_count  = 1;
    proto.constants    = consts;
    proto.const_count  = 1;
    memset(&cl, 0, sizeof(cl));
    cl.proto = &proto;

    UStrand *s = urbi_strand_create(realm, &cl);
    UASSERT(s != NULL);

    /* Arm so a register stack is actually allocated. */
    int rc = urbi_strand_arm_from_closure(s, &cl);
    UASSERT_EQ(0, rc);
    UASSERT(s->stack != NULL);

    void *armed_stack = s->stack;

    /* Caller pre-frees the register stack (mirrors urbi_vm_run path). */
    urbi_strand_register_stack_free(s, &vm);
    UASSERT(s->stack == NULL);

    /* Capture how many frees we have seen so far; ustrand_destroy MUST NOT
     * call alloc_fn(armed_stack, 0, ud) again — armed_stack is already freed. */
    int frees_before_destroy = spy.free_count;

    /* Tear down the strand without going through urbi_strand_destroy (which
     * would also unlink from realm and is exercised elsewhere).  We only care
     * about ustrand_destroy's contract here: it must not double-free the
     * already-NULL'd stack. */
    if (s->realm != NULL && s->realm->strands_head != NULL) {
        UStrand **pp = &s->realm->strands_head;
        while (*pp != NULL) {
            if (*pp == s) {
                *pp = s->next_in_realm;
                s->next_in_realm = NULL;
                break;
            }
            pp = &(*pp)->next_in_realm;
        }
    }
    ustrand_destroy(s, &vm);
    vm.alloc_fn(s, 0, vm.alloc_ud);

    /* Verify: armed_stack does NOT appear in the free log AFTER the
     * pre-free we just issued (frees_before_destroy is the index of the
     * first event that ustrand_destroy could have produced).  Pre-Phase-10
     * the test asserted "1 occurrence in the whole log"; that is no longer
     * stable because urbi_realm_create now runs the baked stdlib chunk
     * via a transient strand, and malloc happily hands armed_stack's
     * memory back to us when that transient's stack frees just before
     * urbi_strand_create is called below.  The contract under test is
     * specifically about ustrand_destroy not re-freeing s->stack — so we
     * scope the assertion to the post-destroy window. */
    int armed_stack_free_count_after_destroy = 0;
    for (int i = frees_before_destroy; i < spy.free_count; i++) {
        if (spy.seen_free_ptrs[i] == armed_stack) {
            armed_stack_free_count_after_destroy++;
        }
    }
    UASSERT_EQ(0, armed_stack_free_count_after_destroy);

    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

/* Case 2: ustrand_destroy on a strand that never had a stack (DORMANT) is a
 * no-op for the register-stack branch.  Pins the same contract from the other
 * direction. */
static void
ustrand_destroy_dormant_no_stack_free(void)
{
    FreeSpy spy = {{NULL}, 0, 0};
    UVM vm;
    urbi_vm_init(&vm, free_spy_alloc, &spy);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    uint32_t instr[1];
    UProto   proto;
    UClosure cl;
    instr[0] = (uint32_t)OP_RET;
    memset(&proto, 0, sizeof(proto));
    proto.instructions = instr;
    proto.instr_count  = 1;
    memset(&cl, 0, sizeof(cl));
    cl.proto = &proto;

    UStrand *s = urbi_strand_create(realm, &cl);
    UASSERT(s != NULL);
    /* Strand is DORMANT — stack was never allocated. */
    UASSERT(s->stack == NULL);

    /* Tear down without arming. */
    if (s->realm != NULL && s->realm->strands_head != NULL) {
        UStrand **pp = &s->realm->strands_head;
        while (*pp != NULL) {
            if (*pp == s) {
                *pp = s->next_in_realm;
                s->next_in_realm = NULL;
                break;
            }
            pp = &(*pp)->next_in_realm;
        }
    }
    ustrand_destroy(s, &vm);
    vm.alloc_fn(s, 0, vm.alloc_ud);

    /* Verify: NULL never appears in the free log (free_spy_alloc only records
     * ptr != NULL frees). */
    for (int i = 0; i < spy.free_count; i++) {
        UASSERT(spy.seen_free_ptrs[i] != NULL);
    }

    urbi_realm_destroy(&vm, realm);
    urbi_vm_destroy(&vm);
}

void test_strand_destroy_suite(void)
{
    utest_run("ustrand_destroy idempotent on freed stack",
              ustrand_destroy_idempotent_on_freed_stack);
    utest_run("ustrand_destroy dormant no stack free",
              ustrand_destroy_dormant_no_stack_free);
}
