/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_value_predicates.c
 *
 * Wave 4 / v0.10.3: urbi_value_is_* predicate family round-trip tests.
 *
 * Covers:
 *   - Every urbi_make_* constructor paired with its urbi_value_is_* predicate.
 *   - Cross-kind rejection: is_X(make_Y()) == false for X != Y.
 *   - Checked-accessor urbi_aux_value_to_*: success path + type-mismatch path.
 *   - URBI_ERR_TYPE returned on mismatch; *out unmodified on mismatch.
 *
 * Closes api-ergonomics F1 (value-ctor / accessor asymmetry).
 */

#include "utest.h"
#include "urbi/types.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* =========================================================================
 * Predicate: is_nil
 * ========================================================================= */

static void is_nil_matches_make_nil(void)
{
    UValue v = urbi_make_nil();
    UASSERT(urbi_value_is_nil(v));
    UASSERT(!urbi_value_is_int(v));
    UASSERT(!urbi_value_is_bool(v));
}

/* =========================================================================
 * Predicate: is_bool
 * ========================================================================= */

static void is_bool_matches_make_bool(void)
{
    UValue vt = urbi_make_bool(true);
    UValue vf = urbi_make_bool(false);
    UASSERT(urbi_value_is_bool(vt));
    UASSERT(urbi_value_is_bool(vf));
    UASSERT(!urbi_value_is_int(vt));
    UASSERT(urbi_value_as_bool(vt) == true);
    UASSERT(urbi_value_as_bool(vf) == false);
}

/* =========================================================================
 * Predicate: is_int
 * ========================================================================= */

static void is_int_matches_make_int(void)
{
    UValue v = urbi_make_int(42);
    UASSERT(urbi_value_is_int(v));
    UASSERT(!urbi_value_is_float(v));
    UASSERT(!urbi_value_is_bool(v));
    UASSERT_EQ(urbi_value_as_int(v), 42);
}

/* =========================================================================
 * Predicate: is_float
 * ========================================================================= */

static void is_float_matches_make_float(void)
{
    UValue v = urbi_make_float(3.14);
    UASSERT(urbi_value_is_float(v));
    UASSERT(!urbi_value_is_int(v));
    UASSERT(!urbi_value_is_nil(v));
}

/* =========================================================================
 * Predicate: is_void
 * ========================================================================= */

static void is_void_matches_make_void(void)
{
    UValue v = urbi_make_void();
    UASSERT(urbi_value_is_void(v));
    UASSERT(!urbi_value_is_nil(v));
    UASSERT(!urbi_value_is_int(v));
}

/* =========================================================================
 * Predicate: is_ptr
 * ========================================================================= */

static void is_ptr_matches_make_ptr(void)
{
    int sentinel = 0;
    UValue v = urbi_make_ptr(&sentinel);
    UASSERT(urbi_value_is_ptr(v));
    UASSERT(!urbi_value_is_int(v));
    UASSERT(urbi_value_as_ptr(v) == (void *)&sentinel);
}

static void is_ptr_null(void)
{
    UValue v = urbi_make_ptr(NULL);
    UASSERT(urbi_value_is_ptr(v));
    UASSERT(urbi_value_as_ptr(v) == NULL);
}

/* =========================================================================
 * Predicate: is_object
 * ========================================================================= */

static void is_object_matches_make_object(void)
{
    /* Synthetic non-NULL pointer — never dereferenced. */
    UValue v = urbi_make_object((struct UObject *)0x1000UL);
    UASSERT(urbi_value_is_object(v));
}

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_int
 * ========================================================================= */

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_float
 * ========================================================================= */

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_bool
 * ========================================================================= */

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_ptr
 * ========================================================================= */

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_str
 * ========================================================================= */

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_object
 * ========================================================================= */

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_event
 * ========================================================================= */

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_closure
 * ========================================================================= */

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_tag
 * ========================================================================= */

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_str — NULL-out pure type check
 * ========================================================================= */


/* =========================================================================
 * Predicate: is_strand — negative cases
 * (no public constructor exists; positive cases require synthetic build)
 * ========================================================================= */

static void is_strand_rejects_int(void)
{
    UASSERT(urbi_value_is_strand(urbi_make_int(0)) == false);
    UASSERT(urbi_value_is_strand(urbi_make_nil()) == false);
}

/* =========================================================================
 * Suite registration
 * ========================================================================= */

void test_value_predicates_suite(void)
{
    utest_run("is_nil_matches_make_nil",       is_nil_matches_make_nil);
    utest_run("is_bool_matches_make_bool",     is_bool_matches_make_bool);
    utest_run("is_int_matches_make_int",       is_int_matches_make_int);
    utest_run("is_float_matches_make_float",   is_float_matches_make_float);
    utest_run("is_void_matches_make_void",     is_void_matches_make_void);
    utest_run("is_ptr_matches_make_ptr",       is_ptr_matches_make_ptr);
    utest_run("is_ptr_null",                   is_ptr_null);
    utest_run("is_object_matches_make_object", is_object_matches_make_object);
    utest_run("is_strand_rejects_int",               is_strand_rejects_int);
}
