/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_make_native_closure.c — Gap L: urbi_make_native_closure
 *
 * TDD tests written before the implementation to pin the public contract:
 *   1. Happy path: valid VM + fn → non-NULL closure with native_fn == fn
 *   2. NULL params: NULL vm or NULL fn → NULL (no crash)
 *   3. OOM: failing allocator → NULL return
 *
 * UClosure fields are accessed via the internal header (test-private peek);
 * the function itself is accessed via the public <urbi/urbi.h>. */

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"              /* UVM typedef */
#include "runtime/uclosure.h"   /* full UClosure definition for field access */

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
 * A minimal host function used as the test target.
 * Signature must match urbi_native_method_fn.
 * ------------------------------------------------------------------------- */

static int test_host_fn(struct UVM *vm, UValue self, UValue *args,
                        uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    *out = urbi_make_nil();
    return 0;
}

/* -------------------------------------------------------------------------
 * Sub-tests
 * ------------------------------------------------------------------------- */

static void make_native_closure_happy_path(void)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(URBI_OK, rc);

    struct UClosure *cl = urbi_make_native_closure(&vm, test_host_fn);
    UASSERT(cl != NULL);
    UASSERT(cl->native_fn == test_host_fn);

    urbi_vm_destroy(&vm);
}

static void make_native_closure_null_vm(void)
{
    /* NULL vm must not crash; expected return is NULL. */
    struct UClosure *cl = urbi_make_native_closure(NULL, test_host_fn);
    UASSERT(cl == NULL);
}

static void make_native_closure_null_fn(void)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(URBI_OK, rc);

    /* NULL fn must not crash; expected return is NULL. */
    struct UClosure *cl = urbi_make_native_closure(&vm, NULL);
    UASSERT(cl == NULL);

    urbi_vm_destroy(&vm);
}

static void make_native_closure_oom(void)
{
    /* Use the failing allocator for the whole VM lifetime.  Pass a large
     * fail_at so urbi_vm_init succeeds; then reset to 0 so the next
     * allocation (inside urbi_make_native_closure) fails.
     *
     * This avoids swapping alloc_fn mid-run — urbi_vm_destroy uses the
     * same alloc_fn to free all heap fields. */
    UVM vm;
    FailAllocCtx ctx = {.fail_at = 99999, .call_count = 0};
    int rc = urbi_vm_init(&vm, fail_alloc, &ctx);
    UASSERT_EQ(URBI_OK, rc);

    /* Reset so the very next allocating call returns NULL. */
    ctx.fail_at = ctx.call_count;

    struct UClosure *cl = urbi_make_native_closure(&vm, test_host_fn);
    UASSERT(cl == NULL);

    /* Reset to pass-through before destroy so teardown frees succeed. */
    ctx.fail_at = 99999;
    urbi_vm_destroy(&vm);
}

/* -------------------------------------------------------------------------
 * Suite entry point
 * ------------------------------------------------------------------------- */

void test_make_native_closure_suite(void)
{
    utest_run("make_native_closure_happy_path",  make_native_closure_happy_path);
    utest_run("make_native_closure_null_vm",     make_native_closure_null_vm);
    utest_run("make_native_closure_null_fn",     make_native_closure_null_fn);
    utest_run("make_native_closure_oom",         make_native_closure_oom);
}
