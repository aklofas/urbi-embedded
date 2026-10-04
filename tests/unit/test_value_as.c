/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_value_as.c — urbi_value_kind + urbi_value_as_* round-trips.
 *
 * Scalar kinds are checked on constructed values; strings, closures,
 * events and tags on values a real VM produced, because those helpers
 * read the runtime's own cell layout. */

#include "utest.h"
#include "urbi/types.h"
#include "urbi/urbi.h"

#include <string.h>
#include <stdint.h>

/* -------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

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

static UValue run_value(UVM *vm, const char *src)
{
    UValue out = urbi_make_nil(); char err[256] = {0};
    if (urbi_run(vm, NULL, src, strlen(src), NULL, &out, err, sizeof err) != URBI_OK) printf("    run: %s\n", err);
    return out;
}

static void a_host_made_string_reads_back_its_bytes(void)
{
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    UValue s = urbi_make_string(vm, "abc", 3);
    UASSERT_EQ((int)urbi_value_kind(s), (int)URBI_VALUE_STR);
    UASSERT(urbi_value_is_str(s));
    size_t n = 99; const char *p = urbi_value_as_str(s, &n);
    UASSERT_EQ(n, 3);
    UASSERT(p && memcmp(p, "abc", 3) == 0 && p[3] == '\0');
    UASSERT(urbi_value_as_str(s, NULL) == p);          /* out_len may be NULL */
    urbi_close(vm);
}

static void a_script_string_literal_is_a_string_to_the_host(void)
{
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    UValue s = run_value(vm, "\"hello\" |");
    UASSERT(urbi_value_is_str(s));
    UASSERT_EQ((int)urbi_value_kind(s), (int)URBI_VALUE_STR);
    size_t n = 0; const char *p = urbi_value_as_str(s, &n);
    UASSERT_EQ(n, 5);
    UASSERT(p && strcmp(p, "hello") == 0);
    UASSERT(urbi_value_as_str(urbi_make_int(1), &n) == NULL);
    UASSERT_EQ(n, 0);
    urbi_close(vm);
}

static void closures_events_and_tags_are_recognised_as_such(void)
{
    UVM *vm = urbi_open(utest_alloc, NULL, NULL);
    UValue f = run_value(vm, "function() { 42 } |");
    UValue e = run_value(vm, "Event.new() |");
    UValue t = run_value(vm, "Tag.new() |");
    UValue l = run_value(vm, "List.new() |");
    UASSERT(urbi_value_is_closure(f) && !urbi_value_is_event(f) && !urbi_value_is_tag(f));
    UASSERT(urbi_value_is_event(e) && !urbi_value_is_closure(e));
    UASSERT(urbi_value_is_tag(t) && !urbi_value_is_event(t));
    UASSERT_EQ((int)urbi_value_kind(f), (int)URBI_VALUE_CLOSURE);
    UASSERT_EQ((int)urbi_value_kind(e), (int)URBI_VALUE_EVENT);
    UASSERT_EQ((int)urbi_value_kind(t), (int)URBI_VALUE_TAG);
    UASSERT_EQ((int)urbi_value_kind(l), (int)URBI_VALUE_CELL);
    UASSERT(!urbi_value_is_closure(l) && !urbi_value_is_closure(urbi_make_int(3)));
    UASSERT(!urbi_value_is_strand(f));
    /* The constructors round-trip through the accessors and are what the
     * runtime accepts back. */
    UValue f2 = urbi_make_closure(urbi_value_as_closure(f));
    UASSERT(urbi_value_is_closure(f2));
    UValue out = urbi_make_nil();
    UASSERT_EQ(URBI_OK, urbi_call(vm, NULL, f2, urbi_make_nil(), NULL, 0, &out));
    UASSERT_EQ(out.v.i, 42);
    UASSERT(urbi_value_is_tag(urbi_make_tag(urbi_value_as_tag(t))));
    UASSERT(urbi_value_is_event(urbi_make_event(urbi_value_as_event(e))));
    urbi_close(vm);
}

void test_value_as_suite(void)
{
    utest_run("a_host_made_string_reads_back_its_bytes", a_host_made_string_reads_back_its_bytes);
    utest_run("a_script_string_literal_is_a_string_to_the_host", a_script_string_literal_is_a_string_to_the_host);
    utest_run("closures_events_and_tags_are_recognised_as_such", closures_events_and_tags_are_recognised_as_such);
    utest_run("kind_of_nil",          kind_of_nil);
    utest_run("kind_of_bool",         kind_of_bool);
    utest_run("kind_of_int",          kind_of_int);
    utest_run("kind_of_float",        kind_of_float);
    utest_run("kind_of_void",         kind_of_void);
    utest_run("kind_of_ptr",          kind_of_ptr);
    utest_run("kind_of_object",       kind_of_object);
    utest_run("as_bool_true",         as_bool_true);
    utest_run("as_bool_false",        as_bool_false);
    utest_run("as_int_roundtrip",     as_int_roundtrip);
    utest_run("as_float_roundtrip",   as_float_roundtrip);
    utest_run("as_ptr_roundtrip",     as_ptr_roundtrip);
    utest_run("as_ptr_null",          as_ptr_null);
    utest_run("as_object_roundtrip",  as_object_roundtrip);
}
