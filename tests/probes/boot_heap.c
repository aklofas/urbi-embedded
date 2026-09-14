/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/probes/boot_heap.c — what a booted VM costs before it runs
 * anything.
 *
 * The design target is a boot heap under 48 KB on a 32-bit target.  That
 * number cannot be measured here: this branch has no multilib and no cross
 * toolchain, and the ports are parked until Phase 5.  What this probe
 * measures is the 64-bit host figure, held under 72 KB — the same ratio,
 * since the dominant cost is pointer-shaped (slot tables, proto arrays,
 * symbol entries) and doubles with the pointer width.  Phase 5 replaces
 * this with the cross-build measurement, and 48 KB becomes the real gate.
 *
 * "Booted" means urbi_open with the standard library installed and the
 * main realm created — everything a host has before it hands the VM a
 * line of script.  It does NOT include a compile: the frontend's arena and
 * the chunk it produces are the program's cost, not the runtime's. */

#include "probe.h"

#define BOOT_HEAP_CAP_HOST (72u * 1024u)

int main(void)
{
    ProbeAlloc a = { 0, 0, 0 };
    UVM *vm = urbi_open(probe_alloc, &a, NULL);
    if (!vm) { fprintf(stderr, "boot_heap: urbi_open failed\n"); return 1; }

    URealm *main_realm = urbi_realm_main(vm);
    if (!main_realm) { fprintf(stderr, "boot_heap: no main realm\n"); urbi_close(vm); return 1; }

    /* Collect first: booting allocates garbage (intermediate strings, the
     * boot strand's stack) that an idle VM does not keep. */
    urbi_gc_collect(vm);

    size_t live = a.live;
    size_t peak = a.peak;
    size_t blocks = a.blocks;
    size_t collected = probe_settled_bytes(vm);

    printf("boot heap:        %lu bytes live in %lu blocks (peak %lu)\n",
           (unsigned long)live, (unsigned long)blocks, (unsigned long)peak);
    printf("  of which GC:    %lu bytes\n", (unsigned long)collected);
    printf("  host cap:       %u bytes\n", BOOT_HEAP_CAP_HOST);

    int ok = live < BOOT_HEAP_CAP_HOST;
    if (!ok)
        fprintf(stderr, "boot_heap: %lu bytes exceeds the %u byte cap\n",
                (unsigned long)live, BOOT_HEAP_CAP_HOST);

    urbi_close(vm);
    if (a.live != 0) {
        fprintf(stderr, "boot_heap: %lu bytes still out on loan after close\n",
                (unsigned long)a.live);
        ok = 0;
    }
    return probe_verdict("boot_heap", ok);
}
