/* SPDX-License-Identifier: BSD-3-Clause */
/* libFuzzer harness for the VM.
 *
 * Feeds raw bytes through uchunk_deserialize; any accepted chunk is
 * executed via uvm_run. Sanitizers (ASan + UBSan) catch undefined
 * behavior, leaks, and crashes in both the dispatch loop and the
 * arithmetic helpers. Most random input is rejected by the loader;
 * only structurally valid chunks reach the VM — which is where the
 * fuzz pressure is useful.
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

#include "uchunk.h"
#include "uvm.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    Chunk chunk = {0};
    if (uchunk_deserialize(&chunk, data, size, NULL, 0) != ULOAD_OK) {
        return 0;
    }

    UVM vm;
    uvm_init(&vm, /* alloc_fn = */ NULL, /* alloc_ud = */ NULL);

    UConst result;
    (void)uvm_run(&vm, &chunk, &result);
    /* Touch result so the compiler keeps the run-path live. */
    if ((int)result.kind < 0) {
        /* unreachable; UValKind is unsigned */
    }

    uvm_destroy(&vm);
    uchunk_destroy(&chunk);
    return 0;
}
