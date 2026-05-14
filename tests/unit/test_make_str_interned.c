/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_make_str_interned.c — Gap N str: urbi_make_str_interned
 *
 * Sub-tests:
 *   1. Intern-equality: same bytes → same pointer (intern semantic).
 *   2. Intern-inequality: different bytes → different pointers.
 *   3. Empty string (len=0): handled safely, returns UVAL_STR.
 *   4. OOM: failing allocator → urbi_make_nil() returned (kind == UVAL_NIL).
 *
 * OOM pattern mirrors test_make_native_closure.c / test_vm_init_oom.c. */

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Failing allocator (same pattern as test_vm_init_oom.c)
 * ------------------------------------------------------------------------- */

typedef struct {
    int fail_at;
    int call_count;
} FailAllocCtx;

static void *fail_alloc(void *ptr, size_t nbytes, void *ud)
{
    FailAllocCtx *c = (FailAllocCtx *)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    if (c->call_count++ == c->fail_at) return NULL;
    return realloc(ptr, nbytes);
}

/* -------------------------------------------------------------------------
 * Sub-tests
 * ------------------------------------------------------------------------- */

static void str_intern_equality(void)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(URBI_OK, rc);

    UValue a = urbi_make_str_interned(&vm, "hello", 5);
    UValue b = urbi_make_str_interned(&vm, "hello", 5);

    /* Both must be UVAL_STR and point to the same canonical address. */
    UASSERT_EQ((int)a.kind, (int)UVAL_STR);
    UASSERT_EQ((int)b.kind, (int)UVAL_STR);
    UASSERT(a.v.p == b.v.p);

    urbi_vm_destroy(&vm);
}

static void str_intern_inequality(void)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(URBI_OK, rc);

    UValue a = urbi_make_str_interned(&vm, "foo", 3);
    UValue b = urbi_make_str_interned(&vm, "bar", 3);

    UASSERT_EQ((int)a.kind, (int)UVAL_STR);
    UASSERT_EQ((int)b.kind, (int)UVAL_STR);
    UASSERT(a.v.p != b.v.p);

    urbi_vm_destroy(&vm);
}

static void str_intern_empty(void)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* Empty string: len=0. Must return UVAL_STR with a NUL-terminated pointer. */
    UValue v = urbi_make_str_interned(&vm, "", 0);
    UASSERT_EQ((int)v.kind, (int)UVAL_STR);
    UASSERT(v.v.p != NULL);
    /* The interned pointer must be NUL-terminated and zero-length. */
    const char *s = (const char *)v.v.p;
    UASSERT_EQ((int)s[0], 0);

    urbi_vm_destroy(&vm);
}

static void str_intern_oom(void)
{
    /* Use the failing allocator.  A large fail_at lets vm_init succeed;
     * we then reset it so the next allocation (intern table entry or grow)
     * returns NULL, causing urbi_make_str_interned to return nil. */
    UVM vm;
    FailAllocCtx ctx = {.fail_at = 99999, .call_count = 0};
    int rc = urbi_vm_init(&vm, fail_alloc, &ctx);
    UASSERT_EQ(URBI_OK, rc);

    /* Force the next allocation to fail — the intern call will try to
     * allocate for the new string entry. */
    ctx.fail_at = ctx.call_count;

    UValue v = urbi_make_str_interned(&vm, "test_oom_string", 15);
    UASSERT_EQ((int)v.kind, (int)UVAL_NIL);

    /* Allow teardown to succeed. */
    ctx.fail_at = 99999;
    urbi_vm_destroy(&vm);
}

/* -------------------------------------------------------------------------
 * Suite entry point
 * ------------------------------------------------------------------------- */

void test_make_str_interned_suite(void)
{
    utest_run("str_intern_equality",   str_intern_equality);
    utest_run("str_intern_inequality", str_intern_inequality);
    utest_run("str_intern_empty",      str_intern_empty);
    utest_run("str_intern_oom",        str_intern_oom);
}
