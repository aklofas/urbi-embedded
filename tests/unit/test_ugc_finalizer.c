/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: UType registration (urbi_register_type) and finalizer
 * invocation via UType.destroy.  Row 10 §7 + §5.3.  T27. */

#include "utest.h"
#include "urbi/gc.h"
#include "ugc_incremental.h"
#include "uvm.h"

#define UTEST(name) static void name(void)

/* === Test helper: static destroy counter ===
 * Each finalizer test uses a file-scope static so the callback can update it. */
static int g_destroy_count = 0;

static void
test_destroy_fn(struct UVM *vm, void *payload)
{
    (void)vm;
    (void)payload;
    g_destroy_count++;
}

/* ===== urbi_register_type tests ===== */

/* Auto-assign (type_tag == 0) must assign a tag >= UTYPE_HOST_BASE. */
UTEST(register_type_auto_assigns_host_slot)
{
    UType t = {0};
    t.name         = "TestType";
    t.payload_size = 16u;
    /* type_tag == 0: auto-assign */

    UVM vm;
    uvm_init(&vm, NULL, NULL);

    uint8_t tag = urbi_register_type(&vm, &t);
    UASSERT(tag >= UTYPE_HOST_BASE);
    UASSERT_EQ(vm.host_type_count, 1u);
    UASSERT(vm.type_table[tag] == &t);

    uvm_destroy(&vm);
}

/* Two successive auto-assigns get distinct tags. */
UTEST(register_two_types_get_distinct_tags)
{
    UType t1 = {0};
    t1.name = "Type1";

    UType t2 = {0};
    t2.name = "Type2";

    UVM vm;
    uvm_init(&vm, NULL, NULL);

    uint8_t tag1 = urbi_register_type(&vm, &t1);
    uint8_t tag2 = urbi_register_type(&vm, &t2);

    UASSERT(tag1 != tag2);
    UASSERT(tag1 >= UTYPE_HOST_BASE);
    UASSERT(tag2 >= UTYPE_HOST_BASE);
    UASSERT_EQ(vm.host_type_count, 2u);
    UASSERT(vm.type_table[tag1] == &t1);
    UASSERT(vm.type_table[tag2] == &t2);

    uvm_destroy(&vm);
}

/* Explicit host tag in [UTYPE_HOST_BASE, UTYPE_HOST_MAX] is used as-is. */
UTEST(register_type_explicit_host_tag)
{
    UType t = {0};
    t.type_tag = UTYPE_HOST_BASE + 5u;
    t.name     = "ExplicitTag";

    UVM vm;
    uvm_init(&vm, NULL, NULL);

    uint8_t tag = urbi_register_type(&vm, &t);
    UASSERT_EQ(tag, UTYPE_HOST_BASE + 5u);
    /* host_type_count is not bumped for explicit tags. */
    UASSERT_EQ(vm.host_type_count, 0u);
    UASSERT(vm.type_table[tag] == &t);

    uvm_destroy(&vm);
}

/* ===== UType.destroy finalizer tests ===== */

/* A dead cell with UGC_HAS_FINALIZER set and a registered UType.destroy
 * must trigger the finalizer during urbi_gc_force_full. */
UTEST(finalizer_runs_on_dead_cell)
{
    UType fin_type = {0};
    fin_type.name         = "FinType";
    fin_type.flags        = TYPE_HAS_FINALIZER;
    fin_type.payload_size = 16u;
    fin_type.destroy      = test_destroy_fn;
    /* type_tag == 0: auto-assign */

    UVM vm;
    uvm_init(&vm, NULL, NULL);

    uint8_t tag = urbi_register_type(&vm, &fin_type);
    UASSERT(tag >= UTYPE_HOST_BASE);

    /* Allocate a cell with the registered type tag. */
    UCell *c = urbi_gc_alloc(&vm, sizeof(UCell) + 16u, tag);
    UASSERT(c != NULL);

    /* Manually set the HAS_FINALIZER bit (urbi_gc_alloc doesn't read UType.flags). */
    c->gc_byte |= UGC_HAS_FINALIZER;

    /* The cell is not reachable from any root — it will be collected. */
    int before = g_destroy_count;
    urbi_gc_force_full(&vm);
    UASSERT_EQ(g_destroy_count, before + 1);

    uvm_destroy(&vm);
}

/* A cell with UGC_HAS_FINALIZER but NO registered type (type_table entry NULL)
 * must not crash — the NULL guard in gc_sweep_step must hold. */
UTEST(finalizer_null_type_no_crash)
{
    UVM vm;
    uvm_init(&vm, NULL, NULL);

    /* Use a built-in type tag that has no UType registered (type_table[1] == NULL). */
    UCell *c = urbi_gc_alloc(&vm, sizeof(UCell) + 8u, UTYPE_OBJECT);
    UASSERT(c != NULL);

    /* Set finalizer bit but leave type_table[UTYPE_OBJECT] == NULL. */
    c->gc_byte |= UGC_HAS_FINALIZER;

    /* Force collection — must not crash. */
    int before = g_destroy_count;
    urbi_gc_force_full(&vm);
    /* No finalizer call (type_table entry is NULL). */
    UASSERT_EQ(g_destroy_count, before);

    uvm_destroy(&vm);
}

/* A pinned cell with UGC_HAS_FINALIZER must NOT have its finalizer called
 * during a sweep (pinned cells are not freed). */
UTEST(finalizer_not_called_for_pinned_cell)
{
    UType fin_type = {0};
    fin_type.name    = "PinnedFinType";
    fin_type.flags   = TYPE_HAS_FINALIZER;
    fin_type.destroy = test_destroy_fn;

    UVM vm;
    uvm_init(&vm, NULL, NULL);

    uint8_t tag = urbi_register_type(&vm, &fin_type);
    UCell *c = urbi_gc_alloc(&vm, sizeof(UCell) + 8u, tag);
    UASSERT(c != NULL);

    c->gc_byte |= UGC_HAS_FINALIZER;
    c->gc_byte |= UGC_IS_PINNED;

    int before = g_destroy_count;
    urbi_gc_force_full(&vm);
    /* Cell is pinned — finalizer must NOT fire. */
    UASSERT_EQ(g_destroy_count, before);

    uvm_destroy(&vm);
}

/* ===== Suite entry point ===== */

void test_ugc_finalizer_suite(void)
{
    utest_run("register_type_auto_assigns_host_slot",
              register_type_auto_assigns_host_slot);
    utest_run("register_two_types_get_distinct_tags",
              register_two_types_get_distinct_tags);
    utest_run("register_type_explicit_host_tag",
              register_type_explicit_host_tag);
    utest_run("finalizer_runs_on_dead_cell",
              finalizer_runs_on_dead_cell);
    utest_run("finalizer_null_type_no_crash",
              finalizer_null_type_no_crash);
    utest_run("finalizer_not_called_for_pinned_cell",
              finalizer_not_called_for_pinned_cell);
}
