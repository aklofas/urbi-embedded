#include "rtest.h"
#include "fakevm.h"
#include <stdlib.h>
static void alloc_and_collect_unrooted(void) {
    struct UVM vm; fakevm_init(&vm, NULL, 0);
    UCell *c = ugc_alloc(&vm, UCELL_STR, 64);
    RT_CHECK(c != NULL && c->type == UCELL_STR && c->size == 64 && vm.gc.cells_live == 1);
    ugc_collect(&vm);
    RT_EQ(vm.gc.cells_live, 0u);
    fakevm_destroy(&vm);
}
static void rooted_survives(void) {
    /* roots[] must be zero-initialized: under URBI_GC_STRESS the very first
     * ugc_alloc collects immediately (before roots[0] is assigned below),
     * and mark_fixed dereferences every slot up to nroots. */
    struct UVM vm; UCell *roots[1] = { NULL };
    fakevm_init(&vm, roots, 1);
    roots[0] = ugc_alloc(&vm, UCELL_HOST, 32);
    UCell *garbage = ugc_alloc(&vm, UCELL_HOST, 32); (void)garbage;
    ugc_collect(&vm);
    RT_EQ(vm.gc.cells_live, 1u);
    RT_CHECK(roots[0]->marked == 0);        /* marks are cleared by sweep */
    fakevm_destroy(&vm);
}
static void threshold_triggers(void) {
    struct UVM vm; fakevm_init(&vm, NULL, 0);
    /* fakevm_init's ustrtab_init raw-allocates the initial bucket array,
     * which already counts toward bytes_since/bytes_live; collect once to
     * fold that into a clean post-collect baseline (bytes_since = 0,
     * bytes_live = raw_live only) before the exact byte math below. */
    ugc_collect(&vm);
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
    fakevm_destroy(&vm);
}
static void mark_is_idempotent_and_iterative(void) {
    struct UVM vm; UCell *roots[1] = { NULL }; fakevm_init(&vm, roots, 1);
    roots[0] = ugc_alloc(&vm, UCELL_HOST, 16);
    ugc_mark(&vm, roots[0]); ugc_mark(&vm, roots[0]);
    RT_EQ(vm.gc.gray_len, 1u);
    ugc_collect(&vm);  /* must not loop or double free */
    RT_EQ(vm.gc.cells_live, 1u);
    fakevm_destroy(&vm);
}
static void raw_memory_tracked(void) {
    struct UVM vm; fakevm_init(&vm, NULL, 0);
    UCell *c = ugc_alloc(&vm, UCELL_STR, 64); (void)c;
    size_t bytes_live_before_raw = vm.gc.bytes_live;
    void *p = ugc_raw_alloc(&vm, 1000);
    RT_CHECK(p != NULL);
    RT_EQ(vm.gc.bytes_live, bytes_live_before_raw + 1000);
    ugc_raw_free(&vm, p, 1000);
    RT_EQ(vm.gc.bytes_live, bytes_live_before_raw);
    fakevm_destroy(&vm);
}
static void pacing_triggers_mid_loop(void) {
    /* Unlike threshold_triggers (which picks the exact count that crosses
     * the threshold with zero internal self-collects, to observe a stable
     * over-threshold state), this test lets ugc_alloc's own maybe_collect
     * actually fire mid-loop and checks the collector recovers cleanly:
     * at least one automatic cycle ran, and far fewer than 40 cells
     * (nothing rooted) are still alive afterward. */
    struct UVM vm; fakevm_init(&vm, NULL, 0);
    vm.gc.threshold = 1024;
    for (int i = 0; i < 40; i++) ugc_alloc(&vm, UCELL_STR, 64);
    RT_CHECK(vm.gc.cycles >= 1u);
    RT_CHECK(vm.gc.cells_live < 40u);
    fakevm_destroy(&vm);
}
typedef struct { UCell hdr; UCell *child; } LinkedCell;
static void linked_trace(struct UVM *vm, UCell *c) {
    LinkedCell *lc = (LinkedCell *)c;
    if (lc->child) ugc_mark(vm, lc->child);
}
static void pinned_cell_traces_child(void) {
    /* Nothing rooted at all: the parent survives only because it's pinned;
     * the child must survive only because tracing the pinned parent finds
     * it -- before the fix, UCELL_F_PINNED bypassed sweep without ever
     * being queued for tracing, so the child was collected out from under
     * a live parent. */
    struct UVM vm; fakevm_init(&vm, NULL, 0);
    vm.gc.hooks.trace = linked_trace;
    LinkedCell *parent = (LinkedCell *)ugc_alloc(&vm, UCELL_HOST, sizeof(LinkedCell));
    /* Pin immediately: under URBI_GC_STRESS the *next* ugc_alloc call
     * collects before allocating, and at that point parent is neither
     * rooted nor (until this line) pinned -- collecting it here then
     * makes the allocator free to hand its memory back for `child`,
     * leaving `parent` dangling for the rest of this function. */
    parent->hdr.flags |= UCELL_F_PINNED;
    LinkedCell *child  = (LinkedCell *)ugc_alloc(&vm, UCELL_HOST, sizeof(LinkedCell));
    parent->child = (UCell *)child;
    ugc_collect(&vm);
    RT_EQ(vm.gc.cells_live, 2u);
    fakevm_destroy(&vm);
}
/* One gray-stack slot is one UCell*; capping every allocation request to
 * the exact byte size of a full 64-entry gray array lets the one initial
 * growth (0 -> 64) succeed but rejects the next doubling (64 -> 128), so
 * the gray stack can never hold more than 64 pending cells at once. */
#define GRAY_CAP_BYTES (64 * sizeof(UCell *))
static void *capped_alloc(void *p, size_t n, void *ud) {
    (void)ud;
    if (n == 0) { free(p); return NULL; }
    if (n > GRAY_CAP_BYTES) return NULL;
    return realloc(p, n);
}
static void gray_overflow_all_survive(void) {
    /* A single 200-node chain never accumulates more than one pending gray
     * entry (pop the parent, push its one child, repeat) -- it can't
     * overflow a gray array of any size, so the overflow path would be
     * unreachable for it. Also, 200 independently rooted *leaf* cells
     * (tried first) pass even against the unfixed collector: a cell that
     * fails to push still gets marked (just never traced), and sweep keeps
     * any nonzero mark -- fine for a leaf with nothing further to reach,
     * so it doesn't actually prove tracing happened. What the unfixed
     * collector gets wrong is specifically the *children* of a cell that
     * overflowed: it's kept alive but its trace() is skipped, so anything
     * reachable only through it is lost. 100 independent 2-cell chains
     * exercise exactly that: each child is reachable only through its own
     * parent, and rooting 100 parents at once means mark_fixed queues more
     * than the capped 64-slot gray stack can hold before any draining
     * starts, so roughly a third of the parents -- and, only via the
     * fallback rescan, their children -- are found after the main drain. */
    struct UVM vm; UCell *roots[100] = { NULL };
    fakevm_init(&vm, roots, 100);
    /* Swap the allocator in after the string table is up: capped_alloc
     * rejects anything larger than one full gray array, which is exactly
     * the size of the initial bucket array, so ordering matters. */
    vm.gc.alloc = capped_alloc;
    vm.gc.hooks.trace = linked_trace;
    for (int i = 0; i < 100; i++) {
        LinkedCell *parent = (LinkedCell *)ugc_alloc(&vm, UCELL_HOST, sizeof(LinkedCell));
        RT_CHECK(parent != NULL);
        /* Root before allocating the child: under URBI_GC_STRESS the next
         * ugc_alloc collects first, and an unrooted, unpinned `parent`
         * would be freed right there (see the identical fix in
         * pinned_cell_traces_child above). */
        roots[i] = (UCell *)parent;
        LinkedCell *child = (LinkedCell *)ugc_alloc(&vm, UCELL_HOST, sizeof(LinkedCell));
        RT_CHECK(child != NULL);
        parent->child = (UCell *)child;
    }
    ugc_collect(&vm);
    RT_EQ(vm.gc.cells_live, 200u);
    fakevm_destroy(&vm);
}
/* A counting allocator with a ceiling: a pacing regression then fails by
 * refusal instead of by growing without bound. */
typedef struct { size_t live, peak, cap; unsigned long refused; } GcAlloc;
static void *gc_counting_alloc(void *ptr, size_t n, void *ud) {
    GcAlloc *a = (GcAlloc *)ud;
    size_t *hdr = ptr ? ((size_t *)ptr) - 2 : NULL;
    size_t old = hdr ? hdr[0] : 0;
    if (n == 0) {
        if (hdr) { a->live -= old; free(hdr); }
        return NULL;
    }
    if (a->cap && a->live - old + n > a->cap) { a->refused++; return NULL; }
    size_t *nh = (size_t *)realloc(hdr, n + 2 * sizeof(size_t));
    if (!nh) return NULL;
    nh[0] = n;
    a->live += n - old;
    if (a->live > a->peak) a->peak = a->live;
    return (void *)(nh + 2);
}
/* Garbage whose raw block is bigger than its cell: every iteration adds
 * both to the allocation count, and a baseline that grew with the raw
 * bytes would stay ahead of that count for ever. */
static void pacing_triggers_for_garbage_that_owns_raw_memory(void) {
    GcAlloc a = { 0, 0, (size_t)1024 * 1024, 0 };
    struct UVM vm; fakevm_init_with(&vm, NULL, 0, gc_counting_alloc, &a);
    uint32_t cycles0 = vm.gc.cycles;
    for (int i = 0; i < 2000; i++) {
        UList *l = (UList *)ugc_alloc(&vm, UCELL_LIST, 64);
        RT_CHECK(l != NULL);
        if (!l) break;
        l->cap = (uint32_t)(256 / sizeof(UValue));
        l->items = (UValue *)ugc_raw_alloc(&vm, (size_t)l->cap * sizeof(UValue));   /* freed by ulist_finalize */
        if (!l->items) l->cap = 0;
        ugc_maybe_collect(&vm);
    }
    RT_CHECK(vm.gc.cycles > cycles0);
    RT_EQ(a.refused, 0ul);
    if (a.peak >= 64u * 1024u) printf("    peak %lu bytes\n", (unsigned long)a.peak);
    RT_CHECK(a.peak < 64u * 1024u);
    fakevm_destroy(&vm);
    RT_EQ(a.live, 0u);
}
static void the_pacing_baseline_does_not_move_between_collections(void) {
    struct UVM vm; fakevm_init(&vm, NULL, 0);
    ugc_collect(&vm);
    size_t base = vm.gc.pace_base, since = vm.gc.bytes_since, raw = vm.gc.raw_live;
    void *p = ugc_raw_alloc(&vm, 10 * 1024);
    RT_CHECK(p != NULL);
    RT_EQ(vm.gc.pace_base, base);
    RT_EQ(vm.gc.bytes_since, since + 10 * 1024);
    RT_EQ(vm.gc.raw_live, raw + 10 * 1024);
    /* And so the raw bytes alone can start a cycle: ten kilobytes is more
     * than twice what a bare VM keeps live. */
    vm.gc.threshold = 1024;
    RT_CHECK(base * 2 < 10 * 1024);
    RT_CHECK(ugc_should_collect(&vm.gc));
    ugc_raw_free(&vm, p, 10 * 1024);
    RT_EQ(vm.gc.pace_base, base);
    fakevm_destroy(&vm);
}
RT_SUITE(rt_gc_suite) {
    rt_run("alloc_and_collect_unrooted", alloc_and_collect_unrooted);
    rt_run("rooted_survives", rooted_survives);
    rt_run("threshold_triggers", threshold_triggers);
    rt_run("mark_is_idempotent_and_iterative", mark_is_idempotent_and_iterative);
    rt_run("raw_memory_tracked", raw_memory_tracked);
    rt_run("pacing_triggers_mid_loop", pacing_triggers_mid_loop);
    rt_run("pinned_cell_traces_child", pinned_cell_traces_child);
    rt_run("gray_overflow_all_survive", gray_overflow_all_survive);
    rt_run("pacing_triggers_for_garbage_that_owns_raw_memory", pacing_triggers_for_garbage_that_owns_raw_memory);
    rt_run("the_pacing_baseline_does_not_move_between_collections", the_pacing_baseline_does_not_move_between_collections);
}
