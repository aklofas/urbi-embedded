/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_value_kind_drift.c — runtime guard for urbi_value_kind_t
 *
 * The compile-time _Static_assert guards in include/urbi/types.h already
 * catch numeric drift at build time.  These runtime checks act as a second
 * line of defence (e.g., if a TU somehow skips the header guard) and
 * document the expected numeric mapping in executable form. */

#include "utest.h"
#include "urbi/types.h"

static void value_kind_int_matches_uval(void)
{
    UASSERT_EQ(URBI_VALUE_INT, UVAL_INT);
}

static void value_kind_float_matches_uval(void)
{
    UASSERT_EQ(URBI_VALUE_FLOAT, UVAL_FLOAT);
}

static void value_kind_bool_matches_uval(void)
{
    UASSERT_EQ(URBI_VALUE_BOOL, UVAL_BOOL);
}

static void value_kind_str_matches_uval(void)
{
    UASSERT_EQ(URBI_VALUE_STR, UVAL_STR);
}

static void value_kind_closure_matches_uval(void)
{
    UASSERT_EQ(URBI_VALUE_CLOSURE, UVAL_CLOSURE);
}

static void value_kind_void_matches_uval(void)
{
    UASSERT_EQ(URBI_VALUE_VOID, UVAL_VOID);
}

static void value_kind_object_matches_uval(void)
{
    UASSERT_EQ(URBI_VALUE_OBJECT, UVAL_OBJECT);
}

static void value_kind_event_matches_uval(void)
{
    UASSERT_EQ(URBI_VALUE_EVENT, UVAL_EVENT);
}

static void value_kind_nil_matches_uval(void)
{
    UASSERT_EQ(URBI_VALUE_NIL, UVAL_NIL);
}

void test_value_kind_drift_suite(void)
{
    utest_run("value_kind_int_matches_uval",     value_kind_int_matches_uval);
    utest_run("value_kind_float_matches_uval",   value_kind_float_matches_uval);
    utest_run("value_kind_bool_matches_uval",    value_kind_bool_matches_uval);
    utest_run("value_kind_str_matches_uval",     value_kind_str_matches_uval);
    utest_run("value_kind_closure_matches_uval", value_kind_closure_matches_uval);
    utest_run("value_kind_void_matches_uval",    value_kind_void_matches_uval);
    utest_run("value_kind_object_matches_uval",  value_kind_object_matches_uval);
    utest_run("value_kind_event_matches_uval",   value_kind_event_matches_uval);
    utest_run("value_kind_nil_matches_uval",     value_kind_nil_matches_uval);
}
