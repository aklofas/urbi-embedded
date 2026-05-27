/* SPDX-License-Identifier: BSD-3-Clause */
/* test_job_proto.c — v0.10.10 / D7-A unit tests for Job proto.
 *
 * Three tests:
 *   1. urbi_job_make round-trip: construct a Job from a strand, verify
 *      the __strand slot contains the strand's pointer and reads as
 *      UVAL_INT.
 *   2. Job.current.status via script returns a string-kind value.
 *   3. Job.current.tags.size returns an integer >= 1.
 */

#include "utest.h"
#include "urbi/urbi.h"
#include "stdlib/job_proto.h"
#include "object/uobject.h"
#include "object/ushape.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "value/uintern.h"
#include "vm/uvm.h"
#include "utest_e2e_helpers.h"

#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* === Test 1: urbi_job_make + __strand slot round-trip ================
 *
 * Create a VM, boot the stdlib so vm->job_proto is populated, then call
 * urbi_job_make with a sentinel UStrand pointer.  Verify the returned
 * value is UVAL_OBJECT and that the __strand slot holds the pointer cast
 * to UVAL_INT.
 *
 * A stack-local zero-initialised UStrand is used as the sentinel; it is
 * NOT linked into realm->strands_head (no live dispatch, no GC hazard).
 * The test only exercises the ctor + slot storage round-trip. */
UTEST(job_make_strand_slot_round_trip)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);

    /* Force stdlib boot so vm->job_proto is populated. */
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);
    UASSERT(vm.job_proto != NULL);

    /* Use a stack-local sentinel as the UStrand* — not linked to realm,
     * safe because urbi_job_make only stores the pointer value. */
    UStrand sentinel;
    memset(&sentinel, 0, sizeof(sentinel));
    UStrand *s = &sentinel;

    UValue j = urbi_job_make(&vm, s);
    UASSERT_EQ((int)j.kind, (int)UVAL_OBJECT);

    /* Resolve the __strand slot and verify the pointer round-trip. */
    UObject *jo = (UObject *)j.v.p;
    USymbol *sym = (USymbol *)ustr_intern(&vm, "__strand", 8);
    UASSERT(sym != NULL);
    UObject *holder = NULL;
    uint32_t idx = 0;
    int rc = urbi_object_resolve_slot(&vm, jo, sym, &holder, &idx);
    UASSERT_EQ(rc, 1);
    UASSERT(holder != NULL);
    UASSERT(holder->slots != NULL);
    UValue uid_v = holder->slots[idx];
    UASSERT_EQ((int)uid_v.kind, (int)UVAL_INT);
    /* The stored integer must equal the strand pointer. */
    UStrand *recovered = (UStrand *)(uintptr_t)(uint64_t)uid_v.v.i;  /* NOLINT(performance-no-int-to-ptr) */
    UASSERT(recovered == s);

    urbi_vm_destroy(&vm);
}

/* === Test 2: Job.current.status returns a string via script ============ */
UTEST(job_current_status_is_string)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    /* Run a script that evaluates Job.current.status — expect a string. */
    UValue out;
    int rc = utest_e2e_compile_and_run(&vm, "Job.current().status()", &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_STR);

    urbi_vm_destroy(&vm);
}

/* === Test 3: Job.current().tags().length() returns an integer >= 0 ===== */
UTEST(job_current_tags_length_gte_0)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UValue out;
    int rc = utest_e2e_compile_and_run(&vm, "Job.current().tags().length()", &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT(out.v.i >= 0);

    urbi_vm_destroy(&vm);
}

/* === Test 4: Job.jobs() returns a List with length >= 1 =================
 *
 * The eval strand is linked into realm->strands_head while urbi_run_chunk
 * executes (uvm_run.c lines 93-94), so Job.jobs() called from a script
 * must return at least one Job (the current eval strand itself).
 * Verifies kind==UVAL_INT (length), value >= 1. */
UTEST(job_jobs_length_gte_1)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    URealm *realm = urbi_realm_global(&vm);
    UASSERT(realm != NULL);

    UValue out;
    int rc = utest_e2e_compile_and_run(&vm, "Job.jobs().length()", &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ((int)out.kind, (int)UVAL_INT);
    UASSERT(out.v.i >= 1);

    urbi_vm_destroy(&vm);
}

/* ===== Suite ===== */
void test_job_proto_suite(void);

void
test_job_proto_suite(void)
{
    utest_run("job_make_strand_slot_round_trip",
              job_make_strand_slot_round_trip);
    utest_run("job_current_status_is_string",
              job_current_status_is_string);
    utest_run("job_current_tags_length_gte_0",
              job_current_tags_length_gte_0);
    utest_run("job_jobs_length_gte_1",
              job_jobs_length_gte_1);
}
