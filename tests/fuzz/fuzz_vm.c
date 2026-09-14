/* SPDX-License-Identifier: BSD-3-Clause */
/* libFuzzer harness for the VM.
 *
 * Feeds raw bytes through the public bytecode-loading entry point; any
 * chunk the loader and verifier accept is then executed.  Sanitizers
 * (ASan + UBSan) catch undefined behaviour, leaks, and crashes in both
 * the dispatch loop and the arithmetic helpers.  Most random input is
 * rejected by the loader; only structurally valid chunks reach the VM,
 * which is where the fuzz pressure is useful.
 *
 * Build:
 *   make fuzz-vm
 *
 * Run:
 *   ./build/host-fuzz/fuzz_vm                 # runs until Ctrl-C
 *   ./build/host-fuzz/fuzz_vm -runs=100000    # bounded smoke test
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "urbi/urbi.h"

static void *fuzz_alloc(void *p, size_t n, void *ud) {
    (void)ud;
    if (n == 0) { free(p); return NULL; }
    return realloc(p, n);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    UVM *vm = urbi_open(fuzz_alloc, NULL, NULL);
    if (vm == NULL) return 0;

    UValue result;
    (void)urbi_load(vm, urbi_realm_main(vm), data, size, &result);
    /* Touch result so the compiler keeps the run path live. */
    if ((int)result.kind < 0) {
        /* unreachable; the kind byte is unsigned */
    }

    urbi_close(vm);
    return 0;
}
