/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: UVM install-time trace fields (spec #2 §5.2).
 *
 * Checks:
 *   - in_watcher_install, trace_overflow, trace_read_set_count are zero at create.
 *   - in_watcher_eval is also zero (mutual-exclusion with install flag).
 *   - URBI_WATCHER_READSET_MAX is defined and >= 4. */

#include "utest.h"
#include "vm/uvm.h"

static void uvm_trace_fields_zero_at_create(void)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    UASSERT_EQ(0, (int)vm.in_watcher_install);
    UASSERT_EQ(0, (int)vm.trace_overflow);
    UASSERT_EQ(0, (int)vm.trace_read_set_count);

    /* Mutually exclusive with M3 in_watcher_eval flag: both zero at create. */
    UASSERT_EQ(0, (int)vm.in_watcher_eval);

    /* Cap macro defined and large enough for minimum useful read-set. */
    UASSERT(URBI_WATCHER_READSET_MAX >= 4);

    uvm_destroy(&vm);
}

void
test_uvm_trace_fields_suite(void)
{
    printf("test_uvm_trace_fields\n");
    utest_run("uvm_trace_fields_zero_at_create",
              uvm_trace_fields_zero_at_create);
}
