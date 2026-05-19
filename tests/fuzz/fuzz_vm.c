/* SPDX-License-Identifier: BSD-3-Clause */
/* libFuzzer harness for the VM.
 *
 * Feeds raw bytes through umodule_deserialize; any accepted module is
 * executed via urbi_vm_run. Sanitizers (ASan + UBSan) catch undefined
 * behavior, leaks, and crashes in both the dispatch loop and the
 * arithmetic helpers. Most random input is rejected by the loader;
 * only structurally valid modules reach the VM — which is where the
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

#include "chunk/umodule.h"
#include "vm/uvm.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    UModule module = {0};
    if (umodule_deserialize(&module, data, size, NULL, 0) != ULOAD_OK) {
        return 0;
    }

    UVM vm;
    urbi_vm_init(&vm, /* alloc_fn = */ NULL, /* alloc_ud = */ NULL);

    UValue result;
    (void)urbi_vm_run(&vm, NULL, &module, &result);
    /* Touch result so the compiler keeps the run-path live. */
    if ((int)result.kind < 0) {
        /* unreachable; UValKind is unsigned */
    }

    urbi_vm_destroy(&vm);
    umodule_destroy(&module, NULL);
    return 0;
}
