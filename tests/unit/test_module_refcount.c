/* SPDX-License-Identifier: BSD-3-Clause */
/* test_module_refcount — direct unit tests for the v0.8.0 UModule refcount
 * mechanism.  Mirrors the v0.7.3 UProto refcount pattern (Piece A of the
 * closure-lifetime spec).
 *
 * Invariant: refcount = (1 per UStrand that has s->module = this module).
 * Bumped at strand binding; decremented at strand destroy.  Module is freed
 * when refcount == 0 AND destroy_requested is true (host has released its
 * reference). */

#include "utest.h"

#include "urbi/urbi.h"
#include "module/umodule.h"
#include "vm/uvm.h"

#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* Task 1: struct fields exist and zero-init correctly. */
UTEST(refcount_fields_zero_initialized)
{
    UModule m = {0};
    UASSERT_EQ((unsigned)0, (unsigned)m.refcount);
    UASSERT_EQ(false, m.destroy_requested);
}

void test_module_refcount_suite(void) {
    utest_run("module_refcount: fields zero-initialized",
              refcount_fields_zero_initialized);
}
