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
#include "urbi/aux.h"

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
    UASSERT(!urbi_value_is_event(v));
    UASSERT(!urbi_value_is_closure(v));
}

/* =========================================================================
 * Predicate: is_event
 * ========================================================================= */

static void is_event_matches_make_event(void)
{
    UValue v = urbi_make_event((struct UEvent *)0x2000UL);
    UASSERT(urbi_value_is_event(v));
    UASSERT(!urbi_value_is_object(v));
    UASSERT(!urbi_value_is_closure(v));
}

/* =========================================================================
 * Predicate: is_closure
 * ========================================================================= */

static void is_closure_matches_make_closure(void)
{
    UValue v = urbi_make_closure((struct UClosure *)0x3000UL);
    UASSERT(urbi_value_is_closure(v));
    UASSERT(!urbi_value_is_event(v));
    UASSERT(!urbi_value_is_object(v));
}

/* =========================================================================
 * Predicate: is_tag
 * ========================================================================= */

static void is_tag_matches_make_tag(void)
{
    UValue v = urbi_make_tag((struct UTag *)0x4000UL);
    UASSERT(urbi_value_is_tag(v));
    UASSERT(!urbi_value_is_object(v));
    UASSERT(!urbi_value_is_nil(v));
}

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_int
 * ========================================================================= */

static void checked_to_int_succeeds_on_int(void)
{
    UValue v = urbi_make_int(123);
    int64_t out = 0;
    int rc = urbi_aux_value_to_int(v, &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ(out, 123);
}

static void checked_to_int_rejects_float(void)
{
    UValue v = urbi_make_float(1.5);
    int64_t out = 999;
    int rc = urbi_aux_value_to_int(v, &out);
    UASSERT_EQ(rc, URBI_ERR_TYPE);
    UASSERT_EQ(out, 999); /* unmodified on mismatch */
}

static void checked_to_int_null_out(void)
{
    /* NULL out acts as a pure type check — must not crash. */
    UValue v = urbi_make_int(7);
    int rc = urbi_aux_value_to_int(v, NULL);
    UASSERT_EQ(rc, URBI_OK);
}

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_float
 * ========================================================================= */

static void checked_to_float_succeeds_on_float(void)
{
    UValue v = urbi_make_float(2.5);
    double out = 0.0;
    int rc = urbi_aux_value_to_float(v, &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT(out == 2.5);
}

static void checked_to_float_rejects_int(void)
{
    UValue v = urbi_make_int(5);
    double out = -1.0;
    int rc = urbi_aux_value_to_float(v, &out);
    UASSERT_EQ(rc, URBI_ERR_TYPE);
    UASSERT(out == -1.0);
}

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_bool
 * ========================================================================= */

static void checked_to_bool_succeeds_on_bool(void)
{
    UValue v = urbi_make_bool(true);
    bool out = false;
    int rc = urbi_aux_value_to_bool(v, &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT(out == true);
}

static void checked_to_bool_rejects_int(void)
{
    UValue v = urbi_make_int(1);
    bool out = false;
    int rc = urbi_aux_value_to_bool(v, &out);
    UASSERT_EQ(rc, URBI_ERR_TYPE);
    UASSERT(out == false); /* unmodified */
}

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_ptr
 * ========================================================================= */

static void checked_to_ptr_succeeds_on_ptr(void)
{
    int dummy = 0;
    UValue v = urbi_make_ptr(&dummy);
    void *out = NULL;
    int rc = urbi_aux_value_to_ptr(v, &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT(out == (void *)&dummy);
}

static void checked_to_ptr_rejects_object(void)
{
    UValue v = urbi_make_object((struct UObject *)0x1000UL);
    void *out = (void *)0x9999UL;
    int rc = urbi_aux_value_to_ptr(v, &out);
    UASSERT_EQ(rc, URBI_ERR_TYPE);
    UASSERT(out == (void *)0x9999UL); /* unmodified */
}

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_str
 * ========================================================================= */

static void checked_to_str_succeeds_on_str(void)
{
    /* No urbi_make_str public ctor (requires live VM); build synthetically
     * matching the UVAL_STR layout documented in types.h:254-261. */
    UValue v;
    v.kind = (uint8_t)UVAL_STR;
    for (size_t i = 0; i < sizeof(v._pad); i++) v._pad[i] = 0;
    v.v.p = (void *)"hello";
    const char *s = NULL;
    size_t len = 999;
    int rc = urbi_aux_value_to_str(v, &s, &len);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT(s != NULL);
    UASSERT_EQ((int)len, 5); /* strlen("hello") via NUL-scan */
}

static void checked_to_str_rejects_int(void)
{
    UValue v = urbi_make_int(42);
    const char *s = (const char *)0xDEADBEEFUL;
    size_t len = 999;
    int rc = urbi_aux_value_to_str(v, &s, &len);
    UASSERT_EQ(rc, URBI_ERR_TYPE);
    UASSERT(s == (const char *)0xDEADBEEFUL); /* unmodified on failure */
    UASSERT_EQ((int)len, 999);                /* unmodified on failure */
}

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_object
 * ========================================================================= */

static void checked_to_object_succeeds_on_object(void)
{
    UValue v = urbi_make_object((struct UObject *)0xDEADBEEFUL);
    struct UObject *out = NULL;
    int rc = urbi_aux_value_to_object(v, &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT(out == (struct UObject *)0xDEADBEEFUL);
}

static void checked_to_object_rejects_float(void)
{
    UValue v = urbi_make_float(1.5);
    struct UObject *out = (struct UObject *)0xCAFEBABEUL;
    int rc = urbi_aux_value_to_object(v, &out);
    UASSERT_EQ(rc, URBI_ERR_TYPE);
    UASSERT(out == (struct UObject *)0xCAFEBABEUL); /* unmodified */
}

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_event
 * ========================================================================= */

static void checked_to_event_succeeds_on_event(void)
{
    UValue v = urbi_make_event((struct UEvent *)0xCAFEBABEUL);
    struct UEvent *out = NULL;
    int rc = urbi_aux_value_to_event(v, &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT(out == (struct UEvent *)0xCAFEBABEUL);
}

static void checked_to_event_rejects_int(void)
{
    UValue v = urbi_make_int(7);
    struct UEvent *out = (struct UEvent *)0xBADF00DUL;
    int rc = urbi_aux_value_to_event(v, &out);
    UASSERT_EQ(rc, URBI_ERR_TYPE);
    UASSERT(out == (struct UEvent *)0xBADF00DUL); /* unmodified */
}

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_closure
 * ========================================================================= */

static void checked_to_closure_succeeds_on_closure(void)
{
    UValue v = urbi_make_closure((struct UClosure *)0xBADF00DUL);
    struct UClosure *out = NULL;
    int rc = urbi_aux_value_to_closure(v, &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT(out == (struct UClosure *)0xBADF00DUL);
}

static void checked_to_closure_rejects_object(void)
{
    UValue v = urbi_make_object((struct UObject *)0x12345UL);
    struct UClosure *out = (struct UClosure *)0x99999UL;
    int rc = urbi_aux_value_to_closure(v, &out);
    UASSERT_EQ(rc, URBI_ERR_TYPE);
    UASSERT(out == (struct UClosure *)0x99999UL); /* unmodified */
}

/* =========================================================================
 * Checked accessor: urbi_aux_value_to_tag
 * ========================================================================= */

static void checked_to_tag_succeeds_on_tag(void)
{
    UValue v = urbi_make_tag((struct UTag *)0xDEADC0DEUL);
    struct UTag *out = NULL;
    int rc = urbi_aux_value_to_tag(v, &out);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT(out == (struct UTag *)0xDEADC0DEUL);
}

static void checked_to_tag_rejects_int(void)
{
    UValue v = urbi_make_int(99);
    struct UTag *out = (struct UTag *)0xABCDEFUL;
    int rc = urbi_aux_value_to_tag(v, &out);
    UASSERT_EQ(rc, URBI_ERR_TYPE);
    UASSERT(out == (struct UTag *)0xABCDEFUL); /* unmodified */
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
    utest_run("is_event_matches_make_event",   is_event_matches_make_event);
    utest_run("is_closure_matches_make_closure", is_closure_matches_make_closure);
    utest_run("is_tag_matches_make_tag",       is_tag_matches_make_tag);
    utest_run("checked_to_int_succeeds",       checked_to_int_succeeds_on_int);
    utest_run("checked_to_int_rejects_float",  checked_to_int_rejects_float);
    utest_run("checked_to_int_null_out",       checked_to_int_null_out);
    utest_run("checked_to_float_succeeds",     checked_to_float_succeeds_on_float);
    utest_run("checked_to_float_rejects_int",  checked_to_float_rejects_int);
    utest_run("checked_to_bool_succeeds",      checked_to_bool_succeeds_on_bool);
    utest_run("checked_to_bool_rejects_int",   checked_to_bool_rejects_int);
    utest_run("checked_to_ptr_succeeds",             checked_to_ptr_succeeds_on_ptr);
    utest_run("checked_to_ptr_rejects_object",       checked_to_ptr_rejects_object);
    utest_run("checked_to_str_succeeds",             checked_to_str_succeeds_on_str);
    utest_run("checked_to_str_rejects_int",          checked_to_str_rejects_int);
    utest_run("checked_to_object_succeeds",          checked_to_object_succeeds_on_object);
    utest_run("checked_to_object_rejects_float",     checked_to_object_rejects_float);
    utest_run("checked_to_event_succeeds",           checked_to_event_succeeds_on_event);
    utest_run("checked_to_event_rejects_int",        checked_to_event_rejects_int);
    utest_run("checked_to_closure_succeeds",         checked_to_closure_succeeds_on_closure);
    utest_run("checked_to_closure_rejects_object",   checked_to_closure_rejects_object);
    utest_run("checked_to_tag_succeeds",             checked_to_tag_succeeds_on_tag);
    utest_run("checked_to_tag_rejects_int",          checked_to_tag_rejects_int);
}
