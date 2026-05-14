/* SPDX-License-Identifier: BSD-3-Clause */
/* test_ref_basic.c — TDD tests for urbi_ref / urbi_ref_get / urbi_unref
 * basic pin/get/unref + generation mismatch (Gap Q, v0.7.1).
 *
 * Four sub-tests:
 *   1. ref_returns_nonzero_handle: urbi_ref returns a non-zero handle.
 *   2. ref_get_returns_pinned_value: urbi_ref_get returns the original value.
 *   3. unref_invalidates_handle: after urbi_unref, urbi_ref_get returns nil.
 *   4. reuse_slot_different_generation: unref then ref again may reuse the
 *      same slot; old handle (stale generation) returns nil; new handle
 *      returns the new value. */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* =========================================================================
 * Sub-test 1: urbi_ref returns a non-zero (non-INVALID) handle.
 * ========================================================================= */

UTEST(ref_returns_nonzero_handle)
{
    UVM vm;
    urbi_ref_t ref;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    ref = urbi_ref(&vm, urbi_make_int(42));
    UASSERT(ref != URBI_REF_INVALID);

    urbi_unref(&vm, ref);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: urbi_ref_get returns the original pinned value.
 * ========================================================================= */

UTEST(ref_get_returns_pinned_value)
{
    UVM vm;
    urbi_ref_t ref;
    UValue v;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    ref = urbi_ref(&vm, urbi_make_int(42));
    UASSERT(ref != URBI_REF_INVALID);

    v = urbi_ref_get(&vm, ref);
    UASSERT_EQ((int)UVAL_INT, (int)v.kind);
    UASSERT_EQ(42LL, (long long)v.v.i);

    urbi_unref(&vm, ref);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: after urbi_unref, urbi_ref_get returns nil (stale handle).
 * ========================================================================= */

UTEST(unref_invalidates_handle)
{
    UVM vm;
    urbi_ref_t ref;
    UValue v;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    ref = urbi_ref(&vm, urbi_make_int(99));
    UASSERT(ref != URBI_REF_INVALID);

    /* Unref the handle — slot freed, generation incremented. */
    urbi_unref(&vm, ref);

    /* Old handle is now stale — must return nil. */
    v = urbi_ref_get(&vm, ref);
    UASSERT_EQ((int)UVAL_NIL, (int)v.kind);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 4: slot reuse with different generation.
 *
 * After urbi_unref, urbi_ref may reuse the same slot (free-list head).
 * The old handle has the old generation; it must return nil.
 * The new handle has the incremented generation; it returns the new value.
 * ========================================================================= */

UTEST(reuse_slot_different_generation)
{
    UVM vm;
    urbi_ref_t ref1, ref2;
    UValue v;

    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    ref1 = urbi_ref(&vm, urbi_make_int(100));
    UASSERT(ref1 != URBI_REF_INVALID);

    /* Unref ref1 — slot goes back on free list. */
    urbi_unref(&vm, ref1);

    /* Allocate again — likely reuses the same slot with incremented gen. */
    ref2 = urbi_ref(&vm, urbi_make_float(3.14));
    UASSERT(ref2 != URBI_REF_INVALID);

    /* Old handle (ref1) must be stale → nil. */
    v = urbi_ref_get(&vm, ref1);
    UASSERT_EQ((int)UVAL_NIL, (int)v.kind);

    /* New handle (ref2) must return the float. */
    v = urbi_ref_get(&vm, ref2);
    UASSERT_EQ((int)UVAL_FLOAT, (int)v.kind);

    /* Even if slot index is different (not same slot due to table growth),
     * ref2 must still return the float. */

    urbi_unref(&vm, ref2);
    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_ref_basic_suite(void)
{
    utest_run("ref_basic: urbi_ref returns non-zero handle",
              ref_returns_nonzero_handle);
    utest_run("ref_basic: urbi_ref_get returns pinned value",
              ref_get_returns_pinned_value);
    utest_run("ref_basic: urbi_unref invalidates handle",
              unref_invalidates_handle);
    utest_run("ref_basic: slot reuse has different generation",
              reuse_slot_different_generation);
}
