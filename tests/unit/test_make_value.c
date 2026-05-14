/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_make_value.c — Gap N: urbi_make_* constructor round-trip
 *
 * Verifies each of the 9 inline constructors by checking:
 *   1. The .kind field is set to the expected UVAL_* / URBI_VALUE_* constant.
 *   2. The payload union arm matches the input value.
 *   3. Pad bytes are zero (canonical bit-identical form, same as urbi_make_nil). */

#include "utest.h"
#include "urbi/types.h"

#include <math.h>
#include <string.h>

/* Helper: verify all pad bytes are zero. */
static int pad_is_zero(const UValue *v)
{
    for (size_t i = 0; i < sizeof(v->_pad); i++) {
        if (v->_pad[i] != 0) return 0;
    }
    return 1;
}

static void make_nil_kind_and_payload(void)
{
    UValue v = urbi_make_nil();
    UASSERT_EQ((int)v.kind, (int)UVAL_NIL);
    UASSERT_EQ(v.v.i, 0);
    UASSERT(pad_is_zero(&v));
}

static void make_bool_true(void)
{
    UValue v = urbi_make_bool(true);
    UASSERT_EQ((int)v.kind, (int)UVAL_BOOL);
    UASSERT_EQ(v.v.i, 1);
    UASSERT(pad_is_zero(&v));
}

static void make_bool_false(void)
{
    UValue v = urbi_make_bool(false);
    UASSERT_EQ((int)v.kind, (int)UVAL_BOOL);
    UASSERT_EQ(v.v.i, 0);
    UASSERT(pad_is_zero(&v));
}

static void make_int_positive(void)
{
    UValue v = urbi_make_int(42);
    UASSERT_EQ((int)v.kind, (int)UVAL_INT);
    UASSERT_EQ(v.v.i, 42);
    UASSERT(pad_is_zero(&v));
}

static void make_int_negative(void)
{
    UValue v = urbi_make_int(-1);
    UASSERT_EQ((int)v.kind, (int)UVAL_INT);
    UASSERT_EQ(v.v.i, -1);
    UASSERT(pad_is_zero(&v));
}

static void make_int_min(void)
{
    int64_t min = (int64_t)0x8000000000000000LL;
    UValue v = urbi_make_int(min);
    UASSERT_EQ((int)v.kind, (int)UVAL_INT);
    UASSERT_EQ(v.v.i, min);
}

static void make_float_basic(void)
{
    UValue v = urbi_make_float(3.14);
    UASSERT_EQ((int)v.kind, (int)UVAL_FLOAT);
    /* Compare via memcmp for exact bit round-trip */
    double expected = 3.14;
    UASSERT(memcmp(&v.v.f, &expected, sizeof(double)) == 0);
    UASSERT(pad_is_zero(&v));
}

static void make_float_zero(void)
{
    UValue v = urbi_make_float(0.0);
    UASSERT_EQ((int)v.kind, (int)UVAL_FLOAT);
    UASSERT(v.v.f == 0.0);
}

static void make_void_kind(void)
{
    UValue v = urbi_make_void();
    UASSERT_EQ((int)v.kind, (int)UVAL_VOID);
    UASSERT_EQ(v.v.i, 0);
    UASSERT(pad_is_zero(&v));
}

static void make_ptr_roundtrip(void)
{
    int sentinel = 0xdeadbeef;
    UValue v = urbi_make_ptr(&sentinel);
    UASSERT_EQ((int)v.kind, (int)URBI_VALUE_PTR);
    UASSERT(v.v.p == (void *)&sentinel);
    UASSERT(pad_is_zero(&v));
}

static void make_ptr_null(void)
{
    UValue v = urbi_make_ptr(NULL);
    UASSERT_EQ((int)v.kind, (int)URBI_VALUE_PTR);
    UASSERT(v.v.p == NULL);
}

static void make_object_roundtrip(void)
{
    /* Use a synthetic non-NULL pointer (cast from integer). */
    struct UObject *fake = (struct UObject *)0x10203040UL;
    UValue v = urbi_make_object(fake);
    UASSERT_EQ((int)v.kind, (int)UVAL_OBJECT);
    UASSERT(v.v.p == (void *)fake);
    UASSERT(pad_is_zero(&v));
}

static void make_event_roundtrip(void)
{
    struct UEvent *fake = (struct UEvent *)0x50607080UL;
    UValue v = urbi_make_event(fake);
    UASSERT_EQ((int)v.kind, (int)UVAL_EVENT);
    UASSERT(v.v.p == (void *)fake);
    UASSERT(pad_is_zero(&v));
}

static void make_closure_roundtrip(void)
{
    struct UClosure *fake = (struct UClosure *)0x90a0b0c0UL;
    UValue v = urbi_make_closure(fake);
    UASSERT_EQ((int)v.kind, (int)UVAL_CLOSURE);
    UASSERT(v.v.p == (void *)fake);
    UASSERT(pad_is_zero(&v));
}

void test_make_value_suite(void)
{
    utest_run("make_nil_kind_and_payload",  make_nil_kind_and_payload);
    utest_run("make_bool_true",             make_bool_true);
    utest_run("make_bool_false",            make_bool_false);
    utest_run("make_int_positive",          make_int_positive);
    utest_run("make_int_negative",          make_int_negative);
    utest_run("make_int_min",               make_int_min);
    utest_run("make_float_basic",           make_float_basic);
    utest_run("make_float_zero",            make_float_zero);
    utest_run("make_void_kind",             make_void_kind);
    utest_run("make_ptr_roundtrip",         make_ptr_roundtrip);
    utest_run("make_ptr_null",              make_ptr_null);
    utest_run("make_object_roundtrip",      make_object_roundtrip);
    utest_run("make_event_roundtrip",       make_event_roundtrip);
    utest_run("make_closure_roundtrip",     make_closure_roundtrip);
}
