/* SPDX-License-Identifier: BSD-3-Clause */
/* test_ref_capacity.c — TDD tests for urbi_ref capacity growth and OOM
 * handling (Gap Q, v0.7.1).
 *
 * Three sub-tests:
 *   1. capacity_grows_for_1000_refs: allocate 1000 refs; all succeed; table
 *      grows transparently; all handles return the original values; clean up.
 *   2. oom_during_grow_returns_invalid: fill initial table slots, then swap
 *      alloc_fn to a null allocator; next urbi_ref (which must grow) returns
 *      URBI_REF_INVALID without corrupting the VM.
 *   3. ref_invalid_equals_zero: URBI_REF_INVALID is 0 (compile-time
 *      verifiable; checked here as a runtime assertion for documentation). */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>  /* free — used by the null-alloc passthrough */

#define UTEST(name) static void name(void)

/* Null allocator: new allocations always fail; frees pass through to free(). */
static void *
ref_null_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return NULL;
}

/* =========================================================================
 * Sub-test 1: allocate 1000 refs — table grows transparently.
 *
 * Initial capacity is 8 (slot 0 reserved → 7 usable).  Each double: 16, 32,
 * 64, 128, 256, 512, 1024, 2048.  1000 refs land comfortably in cap 1024.
 * All handles must return the original value after all refs are taken.
 * ========================================================================= */

#define CAPACITY_TEST_COUNT 1000

UTEST(capacity_grows_for_1000_refs)
{
    UVM vm;
    urbi_ref_t refs[CAPACITY_TEST_COUNT];
    int i;
    UValue v;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* Allocate 1000 int refs. */
    for (i = 0; i < CAPACITY_TEST_COUNT; i++) {
        refs[i] = urbi_ref(&vm, urbi_make_int((long long)i));
        UASSERT(refs[i] != URBI_REF_INVALID);
        if (refs[i] == URBI_REF_INVALID) {
            /* Clean up refs allocated so far on unexpected OOM. */
            int j;
            for (j = 0; j < i; j++) urbi_unref(&vm, refs[j]);
            urbi_vm_destroy(&vm);
            return;
        }
    }

    /* Verify all 1000 handles return the right value. */
    for (i = 0; i < CAPACITY_TEST_COUNT; i++) {
        v = urbi_ref_get(&vm, refs[i]);
        UASSERT_EQ((int)UVAL_INT, (int)v.kind);
        UASSERT_EQ((long long)i, (long long)v.v.i);
    }

    /* Clean up. */
    for (i = 0; i < CAPACITY_TEST_COUNT; i++) {
        urbi_unref(&vm, refs[i]);
    }

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: OOM during table grow → URBI_REF_INVALID, VM not corrupted.
 *
 * Strategy:
 *   1. Init VM with the default allocator.
 *   2. Drain the initial free slots (initial cap=8 → slots 1..7 available).
 *   3. Swap alloc_fn to ref_null_alloc (new allocs fail; frees pass through).
 *   4. Call urbi_ref — no free slots → tries to grow → OOM → URBI_REF_INVALID.
 *   5. Verify the VM is still usable (urbi_ref_get on existing handles works).
 *   6. Restore the real allocator before urbi_vm_destroy (so internal frees
 *      via the stdlib realloc path work correctly).
 *
 * The initial capacity is REF_TABLE_INIT_CAP (== 8, file-private constant),
 * giving 7 usable slots (slot 0 reserved).  We drain exactly those 7 so the
 * next urbi_ref has no free slots left. ========================================================================= */

#define INITIAL_USABLE_SLOTS  7  /* REF_TABLE_INIT_CAP(8) - 1 sentinel */

UTEST(oom_during_grow_returns_invalid)
{
    UVM vm;
    urbi_ref_t refs[INITIAL_USABLE_SLOTS];
    urbi_ref_t extra;
    UVMAllocFn real_alloc_fn;
    void      *real_alloc_ud;
    UValue v;
    int i;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* Drain the initial 7 free slots. */
    for (i = 0; i < INITIAL_USABLE_SLOTS; i++) {
        refs[i] = urbi_ref(&vm, urbi_make_int((long long)(i + 1)));
        UASSERT(refs[i] != URBI_REF_INVALID);
        if (refs[i] == URBI_REF_INVALID) {
            /* Unexpected early OOM — skip rest of test to avoid cascade. */
            int j;
            for (j = 0; j < i; j++) urbi_unref(&vm, refs[j]);
            urbi_vm_destroy(&vm);
            return;
        }
    }

    /* Save real allocator, then swap in the null allocator. */
    real_alloc_fn = vm.alloc_fn;
    real_alloc_ud = vm.alloc_ud;
    vm.alloc_fn   = ref_null_alloc;
    vm.alloc_ud   = NULL;

    /* Next urbi_ref must fail: no free slots + grow fails = URBI_REF_INVALID. */
    extra = urbi_ref(&vm, urbi_make_int(999));
    UASSERT_EQ((urbi_ref_t)URBI_REF_INVALID, extra);

    /* urbi_last_error must report OOM. */
    {
        urbi_error_info_t info;
        int rc = urbi_last_error(&vm, &info);
        UASSERT_EQ((int)URBI_ERR_OOM, rc);
    }

    /* Existing handles still work (VM not corrupted). */
    v = urbi_ref_get(&vm, refs[0]);
    UASSERT_EQ((int)UVAL_INT, (int)v.kind);
    UASSERT_EQ(1LL, (long long)v.v.i);

    /* Restore real allocator before cleanup so urbi_vm_destroy can free. */
    vm.alloc_fn = real_alloc_fn;
    vm.alloc_ud = real_alloc_ud;

    for (i = 0; i < INITIAL_USABLE_SLOTS; i++) {
        urbi_unref(&vm, refs[i]);
    }

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: URBI_REF_INVALID == 0.
 *
 * This is both a compile-time invariant and a runtime documentation check.
 * The encoding (slot_index << 8) | generation with slot 0 permanently
 * reserved means handle 0 == (0 << 8) | 0 == URBI_REF_INVALID.
 * ========================================================================= */

UTEST(ref_invalid_equals_zero)
{
    UASSERT_EQ(0U, (unsigned)URBI_REF_INVALID);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_ref_capacity_suite(void)
{
    utest_run("ref_capacity: 1000 refs → table grows transparently",
              capacity_grows_for_1000_refs);
    utest_run("ref_capacity: OOM during grow → URBI_REF_INVALID",
              oom_during_grow_returns_invalid);
    utest_run("ref_capacity: URBI_REF_INVALID == 0",
              ref_invalid_equals_zero);
}
