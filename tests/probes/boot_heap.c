/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/probes/boot_heap.c — what a booted VM costs before it runs
 * anything.
 *
 * The design target is a boot heap under 48 KB on a 32-bit target, and
 * that is the cap this probe enforces when built for one: it runs on a
 * Cortex-M4 under qemu (`make test-probes-32bit`) and on the boards.  The
 * 64-bit host figure is held under 72 KB as a ratchet; the dominant cost
 * is pointer-shaped (slot tables, proto arrays, symbol entries) and grows
 * with the pointer width, so the host number tracks the 32-bit one.
 *
 * "Booted" means urbi_open with the standard library installed and the
 * main realm created — everything a host has before it hands the VM a
 * line of script.  It does NOT include a compile: the frontend's arena and
 * the chunk it produces are the program's cost, not the runtime's. */

#include "probe.h"

/* 48 KB is the design target on a 32-bit part; the 64-bit host figure is
 * held at 72 KB as a ratchet, since the cost is pointer-shaped. */
#define BOOT_HEAP_CAP ((sizeof(void *) == 4) ? (48u * 1024u) : (72u * 1024u))

int main(void)
{
    ProbeAlloc a = { 0, 0, 0, 0, 0 };
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
    printf("  cap:            %u bytes\n", BOOT_HEAP_CAP);

    int ok = live < BOOT_HEAP_CAP;
    if (!ok)
        fprintf(stderr, "boot_heap: %lu bytes exceeds the %u byte cap\n",
                (unsigned long)live, BOOT_HEAP_CAP);

    urbi_close(vm);
    if (a.live != 0) {
        fprintf(stderr, "boot_heap: %lu bytes still out on loan after close\n",
                (unsigned long)a.live);
        ok = 0;
    }
    return probe_verdict("boot_heap", ok);
}
