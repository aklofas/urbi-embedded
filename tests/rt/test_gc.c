#include "rtest.h"
#include "fakevm.h"
static void alloc_and_collect_unrooted(void) {
    struct UVM vm; fakevm_init(&vm, NULL, 0);
    UCell *c = ugc_alloc(&vm, UCELL_STR, 64);
    RT_CHECK(c != NULL && c->type == UCELL_STR && c->size == 64 && vm.gc.cells_live == 1);
    ugc_collect(&vm);
    RT_EQ(vm.gc.cells_live, 0u);
    ugc_destroy(&vm);
}
static void rooted_survives(void) {
    /* roots[] must be zero-initialized: under URBI_GC_STRESS the very first
     * ugc_alloc collects immediately (before roots[0] is assigned below),
     * and mark_fixed dereferences every slot up to nroots. */
    struct UVM vm; UCell *roots[1] = { NULL };
    fakevm_init(&vm, roots, 1);
    roots[0] = ugc_alloc(&vm, UCELL_OBJ, 32);
    UCell *garbage = ugc_alloc(&vm, UCELL_OBJ, 32); (void)garbage;
    ugc_collect(&vm);
    RT_EQ(vm.gc.cells_live, 1u);
    RT_CHECK(roots[0]->marked == 0);        /* marks are cleared by sweep */
    ugc_destroy(&vm);
}
static void threshold_triggers(void) {
    struct UVM vm; fakevm_init(&vm, NULL, 0);
    vm.gc.threshold = 1024;
    /* 17 * 64 = 1088 is the first multiple of 64 that clears the 1024
     * threshold; ugc_alloc's own maybe_collect call checks *before* each
     * allocation, so any larger count would self-trigger an internal
     * collect partway through the loop (since bytes_live resets to 0 on
     * every collect here, nothing is rooted) and never reach a stable
     * over-threshold state to observe below. */
    for (int i = 0; i < 17; i++) ugc_alloc(&vm, UCELL_STR, 64);
    /* Under URBI_GC_STRESS every alloc already collected, so only the
     * last-iteration cell survives into this check and bytes_since is
     * far below threshold; the pre-check only holds in the paced mode. */
#ifndef URBI_GC_STRESS
    RT_CHECK(ugc_should_collect(&vm.gc));
#endif
    ugc_maybe_collect(&vm);
    RT_EQ(vm.gc.cells_live, 0u);
    RT_CHECK(!ugc_should_collect(&vm.gc));
    ugc_destroy(&vm);
}
static void mark_is_idempotent_and_iterative(void) {
    struct UVM vm; UCell *roots[1] = { NULL }; fakevm_init(&vm, roots, 1);
    roots[0] = ugc_alloc(&vm, UCELL_OBJ, 16);
    ugc_mark(&vm, roots[0]); ugc_mark(&vm, roots[0]);
    RT_EQ(vm.gc.gray_len, 1u);
    ugc_collect(&vm);  /* must not loop or double free */
    RT_EQ(vm.gc.cells_live, 1u);
    ugc_destroy(&vm);
}
static void raw_memory_tracked(void) {
    struct UVM vm; fakevm_init(&vm, NULL, 0);
    UCell *c = ugc_alloc(&vm, UCELL_STR, 64); (void)c;
    size_t cell_only = vm.gc.bytes_live;
    void *p = ugc_raw_alloc(&vm, 1000);
    RT_CHECK(p != NULL);
    RT_EQ(vm.gc.bytes_live, cell_only + 1000);
    ugc_raw_free(&vm, p, 1000);
    RT_EQ(vm.gc.bytes_live, cell_only);
    ugc_destroy(&vm);
}
RT_SUITE(rt_gc_suite) {
    rt_run("alloc_and_collect_unrooted", alloc_and_collect_unrooted);
    rt_run("rooted_survives", rooted_survives);
    rt_run("threshold_triggers", threshold_triggers);
    rt_run("mark_is_idempotent_and_iterative", mark_is_idempotent_and_iterative);
    rt_run("raw_memory_tracked", raw_memory_tracked);
}
