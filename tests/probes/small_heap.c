/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/probes/small_heap.c — does the collector keep a loop alive on a
 * part this small?
 *
 * The allocator refuses anything past the budget on the command line,
 * which is what a 64 KB or 128 KB part does.  The VM is told the same
 * number.  A loop that makes objects, lists, strings and strands must
 * then run ten thousand iterations without one refusal: the collector
 * has to run before the heap is gone, not after. */
#include "probe.h"
int main(int argc, char **argv)
{
    /* Under qemu semihosting the command line is not ours to set, and
     * whatever arrives does not parse as a number: that run gets 64 KB. */
    size_t budget = 64u * 1024u;
    if (argc > 1 && strtoul(argv[1], NULL, 10) > 0) budget = (size_t)strtoul(argv[1], NULL, 10);
    ProbeAlloc a = { 0, 0, 0, 0, 0 };
    a.cap = budget;
    UVMConfig cfg; memset(&cfg, 0, sizeof cfg);
    cfg.boot_stdlib = 1;
    cfg.heap_budget = budget;
    UVM *vm = urbi_open(probe_alloc, &a, &cfg);
    if (!vm) { fprintf(stderr, "small_heap: urbi_open failed under %lu bytes\n", (unsigned long)budget); return 1; }
    int rc = probe_run(vm,
        "var i = 0;"
        " while (i < 10000) {"
        "   var o = Object.new(); o.l = [i, i + 1, i + 2];"
        "   var s = \"a\" + \"b\";"
        "   __detach_strand(function() { 1 });"
        "   i = i + 1"
        " }; i");
    UGcStats st; urbi_gc_stats(vm, &st);
    printf("small_heap: budget %lu, peak %lu, refused %lu, cycles %u\n",
           (unsigned long)budget, (unsigned long)a.peak, (unsigned long)a.refused, (unsigned)st.cycles);
    int ok = rc == URBI_OK && a.refused == 0 && a.peak <= budget;
    urbi_close(vm);
    return probe_verdict("small_heap", ok);
}
