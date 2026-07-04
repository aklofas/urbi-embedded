/* SPDX-License-Identifier: BSD-3-Clause */
/* libFuzzer harness for the bytecode chunk loader (refactor-4 REPL-N1):
 *
 *   - uchunk_deserialize  (src/chunk/uchunk_io.c)  — full loader + verifier
 *
 * The loader is the surface with the actual stack-overflow finding (B3:
 * decode_proto recurses unboundedly); fuzz_json covers the JSON parsers but
 * nothing hit the chunk deserializer.  Target property: no crash / no hang /
 * no sanitizer trip on any byte sequence.  Accepted inputs also exercise the
 * uchunk_destroy ownership path so ASan checks the allocator too.
 *
 * The loader does not need a VM (uchunk_deserialize is self-contained; see
 * uchunk.h comment at the declaration).  Pass NULL for alloc_fn to use the
 * hosted stdlib allocator, and NULL for the vm argument to uchunk_destroy
 * (root was never bound to a VM).
 *
 * Build:
 *   make fuzz-chunk
 *
 * Run:
 *   ./build/host-fuzz/fuzz_chunk tests/fuzz/seeds/chunk/   # Ctrl-C to stop
 *   ./build/host-fuzz/fuzz_chunk tests/fuzz/seeds/chunk/ -runs=20000
 */

#include <stddef.h>
#include <stdint.h>

#include "chunk/uchunk.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    UProto *root = NULL;
    if (uchunk_deserialize(&root, data, size, NULL, NULL, NULL, 0) == UCHUNK_LOAD_OK) {
        /* On success: root is heap-allocated and was never bound to a VM.
         * uchunk_destroy with vm=NULL is the correct paired free. */
        uchunk_destroy(root, NULL);
    }
    /* On failure: internal cleanup has already run (per API contract);
     * root is NULL and nothing is outstanding. */
    return 0;
}
