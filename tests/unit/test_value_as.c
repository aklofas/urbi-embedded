/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_value_as.c — Gap O: urbi_value_kind + urbi_value_as_* round-trip
 *
 * Sub-tests:
 *   1. urbi_value_kind returns the expected URBI_VALUE_* for each constructor.
 *   2. Each urbi_value_as_* accessor returns the original input value.
 *   3. bool round-trip: both true and false.
 *   4. int64_t round-trip: 0, -1, 42, INT64_MIN, INT64_MAX.
 *   5. double round-trip: 0.0, 3.14, checked via bit-exact comparison.
 *   6. ptr round-trip: arbitrary host pointer + NULL.
 *   7. object/event/closure round-trip: synthetic non-NULL pointers.
 *   8. str round-trip: synthetic interned-string pointer (a string literal);
 *      verifies pointer identity + length computation.
 *
 * For the str test, T24's urbi_make_str_interned is not yet available here;
 * the test uses a string literal cast to void* in urbi_make_ptr-style, then
 * stores it directly by building a UVAL_STR UValue with known content. */

#include "utest.h"
#include "urbi/types.h"

#include <string.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/* Build a synthetic UVAL_STR UValue pointing to a string literal.
 * This bypasses interning — used only in test_value_as to exercise the
 * accessor before urbi_make_str_interned lands. */
static UValue synthetic_str(const char *literal)
{
    UValue v = urbi_make_nil();
    v.kind = (uint8_t)UVAL_STR;
    v.v.p = (void *)literal;
    return v;
}

/* -------------------------------------------------------------------------
 * Kind tests
 * ------------------------------------------------------------------------- */

static void kind_of_nil(void)
{
    UValue v = urbi_make_nil();
    UASSERT_EQ((int)urbi_value_kind(v), (int)URBI_VALUE_NIL);
}

static void kind_of_bool(void)
{
    UASSERT_EQ((int)urbi_value_kind(urbi_make_bool(true)),  (int)URBI_VALUE_BOOL);
    UASSERT_EQ((int)urbi_value_kind(urbi_make_bool(false)), (int)URBI_VALUE_BOOL);
}

static void kind_of_int(void)
{
    UASSERT_EQ((int)urbi_value_kind(urbi_make_int(0)),   (int)URBI_VALUE_INT);
    UASSERT_EQ((int)urbi_value_kind(urbi_make_int(-1)),  (int)URBI_VALUE_INT);
    UASSERT_EQ((int)urbi_value_kind(urbi_make_int(42)),  (int)URBI_VALUE_INT);
}

static void kind_of_float(void)
{
    UASSERT_EQ((int)urbi_value_kind(urbi_make_float(0.0)),  (int)URBI_VALUE_FLOAT);
    UASSERT_EQ((int)urbi_value_kind(urbi_make_float(3.14)), (int)URBI_VALUE_FLOAT);
}

static void kind_of_void(void)
{
    UASSERT_EQ((int)urbi_value_kind(urbi_make_void()), (int)URBI_VALUE_VOID);
}

static void kind_of_ptr(void)
{
    UASSERT_EQ((int)urbi_value_kind(urbi_make_ptr(NULL)), (int)URBI_VALUE_PTR);
}

static void kind_of_object(void)
{
    struct UObject *fake = (struct UObject *)0x1000UL;
    UASSERT_EQ((int)urbi_value_kind(urbi_make_object(fake)), (int)URBI_VALUE_OBJECT);
}

static void kind_of_event(void)
{
    struct UEvent *fake = (struct UEvent *)0x2000UL;
    UASSERT_EQ((int)urbi_value_kind(urbi_make_event(fake)), (int)URBI_VALUE_EVENT);
}

static void kind_of_closure(void)
{
    struct UClosure *fake = (struct UClosure *)0x3000UL;
    UASSERT_EQ((int)urbi_value_kind(urbi_make_closure(fake)), (int)URBI_VALUE_CLOSURE);
}

/* -------------------------------------------------------------------------
 * Accessor round-trip tests
 * ------------------------------------------------------------------------- */

static void as_bool_true(void)
{
    UValue v = urbi_make_bool(true);
    UASSERT(urbi_value_as_bool(v) == true);
}

static void as_bool_false(void)
{
    UValue v = urbi_make_bool(false);
    UASSERT(urbi_value_as_bool(v) == false);
}

static void as_int_roundtrip(void)
{
    UASSERT_EQ(urbi_value_as_int(urbi_make_int(0)),   0);
    UASSERT_EQ(urbi_value_as_int(urbi_make_int(-1)), -1);
    UASSERT_EQ(urbi_value_as_int(urbi_make_int(42)),  42);
    /* INT64_MIN / INT64_MAX via literal cast */
    int64_t imin = (int64_t)0x8000000000000000LL;
    int64_t imax = (int64_t)0x7fffffffffffffffLL;
    UASSERT_EQ(urbi_value_as_int(urbi_make_int(imin)), imin);
    UASSERT_EQ(urbi_value_as_int(urbi_make_int(imax)), imax);
}

static void as_float_roundtrip(void)
{
    double zero = 0.0;
    double pi   = 3.14;

    UValue vz = urbi_make_float(zero);
    UValue vp = urbi_make_float(pi);

    /* Bit-exact comparison via memcmp — avoids NaN-comparison UB. */
    double got_z = urbi_value_as_float(vz);
    double got_p = urbi_value_as_float(vp);
    UASSERT(memcmp(&got_z, &zero, sizeof(double)) == 0);
    UASSERT(memcmp(&got_p, &pi,   sizeof(double)) == 0);
}

static void as_ptr_roundtrip(void)
{
    int sentinel = 99;
    UValue v = urbi_make_ptr(&sentinel);
    UASSERT(urbi_value_as_ptr(v) == (void *)&sentinel);
}

static void as_ptr_null(void)
{
    UValue v = urbi_make_ptr(NULL);
    UASSERT(urbi_value_as_ptr(v) == NULL);
}

static void as_object_roundtrip(void)
{
    struct UObject *fake = (struct UObject *)0xabc0UL;
    UValue v = urbi_make_object(fake);
    UASSERT(urbi_value_as_object(v) == fake);
}

static void as_event_roundtrip(void)
{
    struct UEvent *fake = (struct UEvent *)0xdef0UL;
    UValue v = urbi_make_event(fake);
    UASSERT(urbi_value_as_event(v) == fake);
}

static void as_closure_roundtrip(void)
{
    struct UClosure *fake = (struct UClosure *)0xfed0UL;
    UValue v = urbi_make_closure(fake);
    UASSERT(urbi_value_as_closure(v) == fake);
}

static void as_str_roundtrip(void)
{
    /* Synthetic interned-string pointer — bypasses urbi_make_str_interned.
     * Verifies pointer identity and length computation. */
    const char *literal = "hello";
    UValue v = synthetic_str(literal);
    UASSERT_EQ((int)urbi_value_kind(v), (int)URBI_VALUE_STR);

    size_t len = 0;
    const char *got = urbi_value_as_str(v, &len);
    UASSERT(got == literal);
    UASSERT_EQ((int)len, 5);
}

static void as_str_empty(void)
{
    const char *empty = "";
    UValue v = synthetic_str(empty);
    size_t len = 99;
    const char *got = urbi_value_as_str(v, &len);
    UASSERT(got == empty);
    UASSERT_EQ((int)len, 0);
}

static void as_str_null_out_len(void)
{
    /* out_len may be NULL — must not crash. */
    const char *literal = "test";
    UValue v = synthetic_str(literal);
    const char *got = urbi_value_as_str(v, NULL);
    UASSERT(got == literal);
}

void test_value_as_suite(void)
{
    utest_run("kind_of_nil",          kind_of_nil);
    utest_run("kind_of_bool",         kind_of_bool);
    utest_run("kind_of_int",          kind_of_int);
    utest_run("kind_of_float",        kind_of_float);
    utest_run("kind_of_void",         kind_of_void);
    utest_run("kind_of_ptr",          kind_of_ptr);
    utest_run("kind_of_object",       kind_of_object);
    utest_run("kind_of_event",        kind_of_event);
    utest_run("kind_of_closure",      kind_of_closure);
    utest_run("as_bool_true",         as_bool_true);
    utest_run("as_bool_false",        as_bool_false);
    utest_run("as_int_roundtrip",     as_int_roundtrip);
    utest_run("as_float_roundtrip",   as_float_roundtrip);
    utest_run("as_ptr_roundtrip",     as_ptr_roundtrip);
    utest_run("as_ptr_null",          as_ptr_null);
    utest_run("as_object_roundtrip",  as_object_roundtrip);
    utest_run("as_event_roundtrip",   as_event_roundtrip);
    utest_run("as_closure_roundtrip", as_closure_roundtrip);
    utest_run("as_str_roundtrip",     as_str_roundtrip);
    utest_run("as_str_empty",         as_str_empty);
    utest_run("as_str_null_out_len",  as_str_null_out_len);
}
