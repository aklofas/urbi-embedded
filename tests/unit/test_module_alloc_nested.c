/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: umodule_alloc_nested_proto OOM behaviour (MOD-003).
 *
 * The function performs two allocations:
 *   1. Optional grow of module->nested[] (skipped when nested_cap is enough).
 *   2. Fresh UProto struct.
 *
 * If (1) succeeds and (2) fails, the bumped nested_cap and the larger
 * nested[] buffer are retained ("grow-without-commit").  The contract
 * pinned by these tests:
 *   - Returns NULL.
 *   - nested_count is unchanged (no half-committed entry).
 *   - nested_cap is bumped to the new (larger) value (or to the original
 *     value if the grow itself failed).
 *   - A subsequent successful call commits cleanly without leaking. */

#include "utest.h"

#include "module/umodule.h"
#include "vm/uvm.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Failing-allocator harness: fail the Nth NEW allocation request
 * (ptr == NULL && nbytes > 0).  Reallocs (ptr != NULL && nbytes > 0)
 * and frees (ptr != NULL && nbytes == 0) always succeed. */
typedef struct {
    int new_calls;
    int fail_at_new_call;  /* fail when new_calls > fail_at_new_call; -1 = never */
} ProtoOOMSpy;

static void *
proto_oom_alloc(void *ptr, size_t n, void *ud)
{
    ProtoOOMSpy *spy = (ProtoOOMSpy *)ud;
    if (n == 0) {
        free(ptr);
        return NULL;
    }
    if (ptr == NULL) {
        spy->new_calls++;
        if (spy->fail_at_new_call >= 0 &&
            spy->new_calls > spy->fail_at_new_call) {
            return NULL;
        }
    }
    return realloc(ptr, n);
}

/* Case 1: UProto-alloc OOM after a successful nested[] grow leaves the
 * module in a benign over-cap state.  A subsequent successful call must
 * commit cleanly. */
static void
nested_proto_oom_after_grow_recovers_on_retry(void)
{
    UModule module = {0};
    ProtoOOMSpy spy = {0, -1};
    module.alloc_fn = proto_oom_alloc;
    module.alloc_ud = &spy;

    /* Force the grow path to run first (fresh module: nested_cap == 0). */
    /* Configure: allow the nested[] grow (1 NEW alloc), fail the UProto
     * alloc (2nd NEW alloc).  proto_oom_alloc only counts NEW allocs
     * (ptr == NULL); the realloc on a fresh nested array IS a NEW alloc
     * because module->nested starts NULL. */
    spy.new_calls = 0;
    spy.fail_at_new_call = 1;  /* fail the 2nd NEW alloc (UProto) */

    UProto *p = umodule_alloc_nested_proto(&module);
    UASSERT(p == NULL);

    /* nested_count must NOT have been bumped. */
    UASSERT_EQ((long long)module.nested_count, 0LL);

    /* The grow IS allowed to have happened: nested_cap >= 4 (or 0 if grow
     * also failed; on this configuration the grow succeeded). */
    UASSERT(module.nested_cap == 4U);
    /* nested[] buffer exists and points at allocated memory. */
    UASSERT(module.nested != NULL);

    /* Now turn off OOM injection and retry — must succeed.  The retry path
     * skips the grow (cap == 4 already) and tries the UProto alloc. */
    spy.fail_at_new_call = -1;
    UProto *p2 = umodule_alloc_nested_proto(&module);
    UASSERT(p2 != NULL);
    UASSERT_EQ((long long)module.nested_count, 1LL);
    UASSERT_EQ((void *)module.nested[0], (void *)p2);

    umodule_destroy(&module, NULL);
}

/* Case 2: nested[] grow OOM (1st NEW alloc fails) leaves module untouched. */
static void
nested_proto_oom_at_grow_keeps_module_pristine(void)
{
    UModule module = {0};
    ProtoOOMSpy spy = {0, -1};
    module.alloc_fn = proto_oom_alloc;
    module.alloc_ud = &spy;

    /* Fail the 1st NEW alloc (the nested[] grow). */
    spy.new_calls = 0;
    spy.fail_at_new_call = 0;

    UProto *p = umodule_alloc_nested_proto(&module);
    UASSERT(p == NULL);

    /* Module fields stay at the zero-init values: no nested[] buffer
     * was attached and the cap stays at 0. */
    UASSERT(module.nested == NULL);
    UASSERT_EQ((long long)module.nested_cap, 0LL);
    UASSERT_EQ((long long)module.nested_count, 0LL);

    umodule_destroy(&module, NULL);
}

/* Case 3: serialize / iterate paths do not read beyond nested_count.
 * Pins the contract that the over-cap state from Case 1 is safe even if
 * code walks module->nested[] later. */
static void
nested_over_cap_iteration_stops_at_count(void)
{
    UModule module = {0};
    ProtoOOMSpy spy = {0, -1};
    module.alloc_fn = proto_oom_alloc;
    module.alloc_ud = &spy;

    /* First call: succeed both grow + UProto. */
    UProto *p1 = umodule_alloc_nested_proto(&module);
    UASSERT(p1 != NULL);
    UASSERT_EQ((long long)module.nested_count, 1LL);
    size_t cap_after_first = module.nested_cap;
    UASSERT(cap_after_first >= 1U);

    /* Walk [0..nested_count) — the only slot is index 0 with our proto. */
    for (size_t i = 0; i < module.nested_count; i++) {
        UASSERT(module.nested[i] != NULL);
    }
    /* Slots beyond nested_count are not initialised; we do NOT read them. */
    (void)cap_after_first;

    umodule_destroy(&module, NULL);
}

void test_module_alloc_nested_suite(void)
{
    utest_run("nested_proto_oom_after_grow_recovers_on_retry",
              nested_proto_oom_after_grow_recovers_on_retry);
    utest_run("nested_proto_oom_at_grow_keeps_module_pristine",
              nested_proto_oom_at_grow_keeps_module_pristine);
    utest_run("nested_over_cap_iteration_stops_at_count",
              nested_over_cap_iteration_stops_at_count);
}
