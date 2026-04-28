/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: strand allocator exhaustion (row 12 §8.2 F-5 counterexample).
 *
 * There is no fixed URBI_STRAND_POOL_SIZE cap at M3; strands are allocated
 * through the VM's pluggable alloc_fn.  The F-5 counterexample is therefore:
 * when alloc_fn returns NULL, urbi_strand_create returns NULL (not a crash or
 * assertion failure), allowing the host to degrade gracefully.
 *
 * Three tests:
 *   (1) strand_create_null_alloc_returns_null:
 *       urbi_strand_create returns NULL when the internal allocator fails.
 *       Implemented by patching the VM's alloc_fn after realm creation so
 *       that only the UStrand allocation itself fails.
 *
 *   (2) strand_alloc_exhaustion_returns_null:
 *       A counting allocator allows N allocs (calibrated at runtime) then
 *       fails; urbi_strand_create returns NULL without corrupting the VM.
 *
 *   (3) strand_dormant_destroy_no_counter_corrupt:
 *       Destroying a DORMANT strand whose cleanup_base is NULL (partial init)
 *       does not underflow the scheduler's liveness counters.
 */

#include "utest.h"
#include "uvm.h"
#include "urealm.h"
#include "ustrand.h"
#include "urbi.h"
#include "usched_cooperative.h"

#include <string.h>
#include <stddef.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* ---- Null allocator: always fails new allocations; passes frees ---- */

static void *
pool_null_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return NULL;  /* allocation always fails */
}

/* ---- Counting allocator: fail after `limit` successful allocations ---- */

typedef struct {
    size_t   count;   /* total allocations so far */
    size_t   limit;   /* fail when count >= limit */
} CountAlloc;

static void *
count_alloc(void *ptr, size_t nbytes, void *ud)
{
    CountAlloc *ca = (CountAlloc *)ud;
    if (nbytes == 0) {
        free(ptr);
        return NULL;
    }
    if (ca->count >= ca->limit) return NULL;
    ca->count++;
    return malloc(nbytes);
}

/* ===========================================================================
 * Tests
 * =========================================================================== */

/* Case 1: urbi_strand_create returns NULL when alloc_fn fails for the strand.
 *
 * Create the realm with a working allocator, then swap alloc_fn to one that
 * always fails new allocations so only the UStrand's own allocation fails.
 * Restore the working allocator before cleanup. */
UTEST(strand_create_null_alloc_returns_null)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);  /* NULL = stdlib realloc shim installed */

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    /* Save and replace: next alloc fails. */
    UVMAllocFn saved_alloc = vm.alloc_fn;
    void       *saved_ud   = vm.alloc_ud;
    vm.alloc_fn = pool_null_alloc;
    vm.alloc_ud = NULL;

    UStrand *s = urbi_strand_create(realm, NULL);
    UASSERT(s == NULL);  /* allocation failure must return NULL, not crash */

    /* Restore so destroy can free memory. */
    vm.alloc_fn = saved_alloc;
    vm.alloc_ud = saved_ud;

    urbi_realm_destroy(&vm, realm);
    uvm_destroy(&vm);
}

/* Case 2: counting allocator — after N strands the N+1-th creation returns NULL.
 *
 * The limit is calibrated at runtime: we count allocs consumed by uvm_init and
 * urbi_realm_create, then allow exactly one more (for the first strand).  The
 * second strand creation must fail gracefully. */
UTEST(strand_alloc_exhaustion_returns_null)
{
    CountAlloc ca = {0, (size_t)-1};  /* unlimited initially */
    UVM vm;
    uvm_init(&vm, count_alloc, &ca);

    URealm *realm = urbi_realm_create(&vm);
    UASSERT(realm != NULL);

    /* Create one strand successfully. */
    UStrand *s1 = urbi_strand_create(realm, NULL);
    UASSERT(s1 != NULL);

    /* Cap at current count: next allocation fails. */
    ca.limit = ca.count;

    /* Second strand must return NULL. */
    UStrand *s2 = urbi_strand_create(realm, NULL);
    UASSERT(s2 == NULL);

    /* First strand and VM remain valid. */
    UASSERT(s1->vm == &vm);
    UASSERT(USTRAND_GET_STATE(s1) == USTRAND_DORMANT);
    UASSERT_EQ(vm.strand_runnable_count, 0u);

    /* Re-enable allocs for cleanup. */
    ca.limit = (size_t)-1;

    urbi_strand_destroy(s1);
    urbi_realm_destroy(&vm, realm);
    uvm_destroy(&vm);
}

/* Case 3: destroying a partially-initialised DORMANT strand (cleanup_base == NULL)
 * does not underflow the scheduler's liveness counters.
 *
 * This covers the path where ustrand_init fails to allocate the cleanup stack
 * (alloc_fn returns NULL during init) and the caller tears down the struct. */
UTEST(strand_dormant_destroy_no_counter_corrupt)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    /* ustrand_init with a NULL-alloc vm: cleanup_base stays NULL. */
    UVMAllocFn saved_alloc = vm.alloc_fn;
    void       *saved_ud   = vm.alloc_ud;
    vm.alloc_fn = pool_null_alloc;
    vm.alloc_ud = NULL;

    UStrand s;
    ustrand_init(&s, &vm);  /* cleanup_base == NULL (alloc failed) */
    UASSERT(s.cleanup_base == NULL);

    /* Restore before destroy so that any internal free paths work. */
    vm.alloc_fn = saved_alloc;
    vm.alloc_ud = saved_ud;

    ustrand_destroy(&s, &vm);

    /* Counters must be clean. */
    UASSERT_EQ(vm.strand_runnable_count, 0u);
    UASSERT_EQ(vm.wakeup_pending_count,  0u);
    UASSERT_EQ(vm.host_call_pending_count, 0u);

    uvm_destroy(&vm);
}

/* ===========================================================================
 * Suite registration
 * =========================================================================== */

void test_sched_pool_exhaust_suite(void)
{
    utest_run("strand_create_null_alloc_returns_null",
              strand_create_null_alloc_returns_null);
    utest_run("strand_alloc_exhaustion_returns_null",
              strand_alloc_exhaustion_returns_null);
    utest_run("strand_dormant_destroy_no_counter_corrupt",
              strand_dormant_destroy_no_counter_corrupt);
}
