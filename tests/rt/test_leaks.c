/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/rt/test_leaks.c — does running a program twice cost twice the
 * memory?
 *
 * The pre-refoundation core answered yes for containers: a List's backing
 * buffer lived outside the heap, threaded onto a VM-lifetime list and
 * freed only at urbi_vm_destroy, so a loop that built a thousand short-
 * lived lists held all thousand until the VM went away.  Nothing in the
 * suite noticed, because nothing compared the heap before and after.
 *
 * Each probe here runs the same program twice with a full collection
 * after each, and asserts the live-byte count barely moved between the
 * two.  The FIRST run is allowed to cost whatever it costs — it interns
 * the program's symbols, binds its chunk and creates its globals.  The
 * SECOND run adds nothing new, so a difference between the two is
 * something the first run allocated and never gave back, scaled by
 * however many iterations the loop ran.
 *
 * A kilobyte of slack covers the one legitimate difference: the second
 * run's chunk is bound while the first run's is still reachable from the
 * global it defined, so the two chunk cells briefly coexist.
 *
 * These go through the PUBLIC API rather than fakevm, because the leak
 * being pinned is the one an embedder would see.
 *
 * bytes_live is a number the collector WRITES, not one the allocator
 * tracks continuously: it is the cell bytes that survived the last cycle
 * plus the raw arrays those cells still own.  Reading it without
 * collecting first would report the previous cycle's answer, so every
 * probe collects explicitly before it looks. */

#include <stdlib.h>
#include <string.h>

#include "rtest.h"
#include "urbi/urbi.h"

/* Allowance per probe: bigger than the chunk-cell overlap, far smaller
 * than a thousand leaked cells. */
#define LEAK_SLACK 1024u

static void *plain_alloc(void *ptr, size_t n, void *ud)
{
    (void)ud;
    if (n == 0) { free(ptr); return NULL; }
    return realloc(ptr, n);
}

/* Runs `src` once and reports live bytes after a full collection.
 * Returns 0 and reports the failure itself when the program did not run. */
static size_t run_and_settle(UVM *vm, const char *src, const char *label)
{
    UValue out = urbi_make_nil();
    char err[256] = { 0 };
    int rc = urbi_run(vm, urbi_realm_main(vm), src, strlen(src), "<leak>", &out, err, sizeof err);
    if (rc != URBI_OK) {
        UErrorInfo info;
        urbi_last_error(vm, &info);
        printf("    %s failed rc=%d err=%s last=%s\n", label, rc, err,
               info.message ? info.message : "");
    }
    RT_EQ(rc, URBI_OK);
    urbi_gc_collect(vm);
    UGcStats st;
    urbi_gc_stats(vm, &st);
    return st.bytes_live;
}

/* The shared body: two runs, one comparison, one diagnostic line when the
 * second run cost more than the slack allows. */
static void probe(const char *label, const char *src)
{
    UVM *vm = urbi_open(plain_alloc, NULL, NULL);
    RT_CHECK(vm != NULL);
    if (!vm) return;

    size_t after1 = run_and_settle(vm, src, label);
    size_t after2 = run_and_settle(vm, src, label);
    size_t delta = after2 > after1 ? after2 - after1 : after1 - after2;

    if (delta >= LEAK_SLACK)
        printf("    %s: %lu -> %lu bytes live, delta %lu (slack %u)\n", label,
               (unsigned long)after1, (unsigned long)after2,
               (unsigned long)delta, (unsigned)LEAK_SLACK);
    RT_CHECK(delta < LEAK_SLACK);
    urbi_close(vm);
}

/* A thousand two-element lists, each dead by the next iteration. */
static void lists_do_not_accumulate(void)
{
    probe("lists",
          "var g = function(n) { var k = 0; while (k < n) { var l = [k, k]; k = k + 1 }; k }; g(1000)");
}

/* A thousand concatenations.  Strings were already GC cells before this
 * task; the probe is here so the two kinds are held to one standard. */
static void strings_do_not_accumulate(void)
{
    probe("strings",
          "var g = function(n) { var k = 0; while (k < n) { var s = \"x\" + k.asString(); k = k + 1 }; k }; g(1000)");
}

/* A thousand clones of the Object root. */
static void clones_do_not_accumulate(void)
{
    probe("clones",
          "var g = function(n) { var k = 0; while (k < n) { var o = Object.clone(); k = k + 1 }; k }; g(1000)");
}

/* One dict of a thousand entries, dropped when the function returns.  The
 * keys and values go with it, so this probes the key/value arrays as well
 * as the cell. */
static void dicts_do_not_accumulate(void)
{
    probe("dict",
          "var g = function(n) { var d = Dict.new(); var k = 0;"
          " while (k < n) { d.set(k.asString(), k); k = k + 1 }; d.length() }; g(1000)");
}

/* The lists probe with the result KEPT: a thousand-element list held by a
 * global has to cost something, or the probes above would pass on a VM
 * that simply never allocated.  Both runs keep exactly one such list, so
 * the second still adds nothing. */
static void a_retained_list_costs_memory(void)
{
    UVM *vm = urbi_open(plain_alloc, NULL, NULL);
    RT_CHECK(vm != NULL);
    if (!vm) return;

    urbi_gc_collect(vm);
    UGcStats before;
    urbi_gc_stats(vm, &before);

    const char *src = "var kept = List.new(); var k = 0;"
                      " while (k < 1000) { kept.add(k); k = k + 1 }; kept.length()";
    size_t after1 = run_and_settle(vm, src, "retained");
    RT_CHECK(after1 > before.bytes_live + 1000u * sizeof(UValue) / 2u);

    size_t after2 = run_and_settle(vm, src, "retained");
    size_t delta = after2 > after1 ? after2 - after1 : after1 - after2;
    if (delta >= LEAK_SLACK)
        printf("    retained: %lu -> %lu bytes live, delta %lu\n",
               (unsigned long)after1, (unsigned long)after2, (unsigned long)delta);
    RT_CHECK(delta < LEAK_SLACK);
    urbi_close(vm);
}

RT_SUITE(rt_leaks_suite) {
    rt_run("lists_do_not_accumulate", lists_do_not_accumulate);
    rt_run("strings_do_not_accumulate", strings_do_not_accumulate);
    rt_run("clones_do_not_accumulate", clones_do_not_accumulate);
    rt_run("dicts_do_not_accumulate", dicts_do_not_accumulate);
    rt_run("a_retained_list_costs_memory", a_retained_list_costs_memory);
}
