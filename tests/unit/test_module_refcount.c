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

/* Task 2: inc/dec helpers mutate refcount correctly. */
UTEST(refcount_inc_dec_basic)
{
    UModule m = {0};
    umodule_refcount_inc(&m, NULL);
    UASSERT_EQ((unsigned)1, (unsigned)m.refcount);
    umodule_refcount_inc(&m, NULL);
    UASSERT_EQ((unsigned)2, (unsigned)m.refcount);
    umodule_refcount_dec(&m, NULL);
    UASSERT_EQ((unsigned)1, (unsigned)m.refcount);
    umodule_refcount_dec(&m, NULL);
    UASSERT_EQ((unsigned)0, (unsigned)m.refcount);
}

UTEST(refcount_inc_saturates_at_uint16_max)
{
    UModule m = {0};
    m.refcount = UINT16_MAX;
    umodule_refcount_inc(&m, NULL);
    UASSERT_EQ((unsigned)UINT16_MAX, (unsigned)m.refcount);
    /* No crash, no wrap.  Saturation policy matches v0.7.3 UProto. */
}

void test_module_refcount_suite(void) {
    utest_run("module_refcount: fields zero-initialized",
              refcount_fields_zero_initialized);
    utest_run("module_refcount: inc/dec basic",
              refcount_inc_dec_basic);
    utest_run("module_refcount: inc saturates at UINT16_MAX",
              refcount_inc_saturates_at_uint16_max);
}
