/* SPDX-License-Identifier: BSD-3-Clause */
/* test_aux_dump_value.c — TDD tests for urbi_aux_dump_value per-kind
 * formatting (Phase 9, v0.7.1).
 *
 * Sub-tests per value kind:
 *   1. nil   → "nil"
 *   2. bool  → "true" / "false"
 *   3. int   → decimal string
 *   4. float → %g form
 *   5. str   → double-quoted with escapes
 *   6. void  → "void"
 *   7. ptr   → "ptr@0x..."
 *   8. truncation: buf_size < required → truncates, NUL-terminated
 *   9. return value semantics: returns count as snprintf */

#include "utest.h"
#include "urbi/aux.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"

#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define UTEST(name) static void name(void)

/* =========================================================================
 * Sub-test 1: URBI_VALUE_NIL → "nil"
 * ========================================================================= */

UTEST(dump_nil)
{
    char buf[64];
    int n = urbi_aux_dump_value(NULL, urbi_make_nil(), buf, sizeof buf);
    UASSERT(strcmp(buf, "nil") == 0);
    UASSERT_EQ(3, n);
}

/* =========================================================================
 * Sub-test 2: URBI_VALUE_BOOL → "true" / "false"
 * ========================================================================= */

UTEST(dump_bool_true)
{
    char buf[64];
    int n = urbi_aux_dump_value(NULL, urbi_make_bool(true), buf, sizeof buf);
    UASSERT(strcmp(buf, "true") == 0);
    UASSERT_EQ(4, n);
}

UTEST(dump_bool_false)
{
    char buf[64];
    int n = urbi_aux_dump_value(NULL, urbi_make_bool(false), buf, sizeof buf);
    UASSERT(strcmp(buf, "false") == 0);
    UASSERT_EQ(5, n);
}

/* =========================================================================
 * Sub-test 3: URBI_VALUE_INT → decimal
 * ========================================================================= */

UTEST(dump_int)
{
    char buf[64];
    int n = urbi_aux_dump_value(NULL, urbi_make_int(42), buf, sizeof buf);
    UASSERT(strcmp(buf, "42") == 0);
    UASSERT_EQ(2, n);
}

UTEST(dump_int_negative)
{
    char buf[64];
    urbi_aux_dump_value(NULL, urbi_make_int(-99), buf, sizeof buf);
    UASSERT(strcmp(buf, "-99") == 0);
}

/* =========================================================================
 * Sub-test 4: URBI_VALUE_FLOAT → %g form
 * ========================================================================= */

UTEST(dump_float)
{
    char buf[64];
    char expected[64];
    double f = 3.14;
    snprintf(expected, sizeof expected, "%g", f);
    urbi_aux_dump_value(NULL, urbi_make_float(f), buf, sizeof buf);
    UASSERT(strcmp(buf, expected) == 0);
}

/* =========================================================================
 * Sub-test 5: URBI_VALUE_STR → double-quoted with escapes
 * ========================================================================= */

UTEST(dump_str_simple)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UValue v = urbi_make_str_interned(&vm, "hello", 5);
    char buf[64];
    urbi_aux_dump_value(&vm, v, buf, sizeof buf);
    UASSERT(strcmp(buf, "\"hello\"") == 0);

    urbi_vm_destroy(&vm);
}

UTEST(dump_str_with_escapes)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    /* String containing newline + tab + backslash + quote */
    const char raw[] = "a\nb\\\"c";
    UValue v = urbi_make_str_interned(&vm, raw, sizeof raw - 1);
    char buf[128];
    urbi_aux_dump_value(&vm, v, buf, sizeof buf);
    /* Expected: "a\nb\\\"c" */
    UASSERT(strcmp(buf, "\"a\\nb\\\\\\\"c\"") == 0);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 6: URBI_VALUE_VOID → "void"
 * ========================================================================= */

UTEST(dump_void)
{
    char buf[64];
    int n = urbi_aux_dump_value(NULL, urbi_make_void(), buf, sizeof buf);
    UASSERT(strcmp(buf, "void") == 0);
    UASSERT_EQ(4, n);
}

/* =========================================================================
 * Sub-test 7: URBI_VALUE_PTR → "ptr@0x..."
 * ========================================================================= */

UTEST(dump_ptr)
{
    char buf[128];
    void *p = (void *)0x1234;
    urbi_aux_dump_value(NULL, urbi_make_ptr(p), buf, sizeof buf);
    UASSERT(strncmp(buf, "ptr@", 4) == 0);
}

/* =========================================================================
 * Sub-test 8: buffer too small → truncated but NUL-terminated.
 * ========================================================================= */

UTEST(dump_truncation)
{
    char buf[4];  /* "nil" + NUL fits; "false" does not */
    urbi_aux_dump_value(NULL, urbi_make_bool(false), buf, sizeof buf);
    /* Must be NUL-terminated within buf. */
    UASSERT(buf[sizeof buf - 1] == '\0' || strlen(buf) < sizeof buf);
    /* At most 3 bytes of content ("fal" or similar). */
    UASSERT(strlen(buf) < sizeof buf);
}

/* =========================================================================
 * Sub-test 9: return value semantics (snprintf convention).
 * ========================================================================= */

UTEST(dump_return_value)
{
    char buf[64];
    /* "nil" → return 3 */
    int n = urbi_aux_dump_value(NULL, urbi_make_nil(), buf, sizeof buf);
    UASSERT_EQ(3, n);

    /* "true" → return 4 */
    n = urbi_aux_dump_value(NULL, urbi_make_bool(true), buf, sizeof buf);
    UASSERT_EQ(4, n);
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_aux_dump_value_suite(void)
{
    utest_run("aux_dump_value: nil → \"nil\"",               dump_nil);
    utest_run("aux_dump_value: bool true → \"true\"",         dump_bool_true);
    utest_run("aux_dump_value: bool false → \"false\"",       dump_bool_false);
    utest_run("aux_dump_value: int 42 → \"42\"",              dump_int);
    utest_run("aux_dump_value: int negative",                  dump_int_negative);
    utest_run("aux_dump_value: float → %g form",              dump_float);
    utest_run("aux_dump_value: str simple → quoted",          dump_str_simple);
    utest_run("aux_dump_value: str with escapes",             dump_str_with_escapes);
    utest_run("aux_dump_value: void → \"void\"",              dump_void);
    utest_run("aux_dump_value: ptr → \"ptr@0x...\"",          dump_ptr);
    utest_run("aux_dump_value: truncation NUL-terminated",    dump_truncation);
    utest_run("aux_dump_value: return value snprintf semantics",
              dump_return_value);
}
