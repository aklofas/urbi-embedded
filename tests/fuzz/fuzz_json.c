/* SPDX-License-Identifier: BSD-3-Clause */
/* libFuzzer harness for the two network-facing JSON parsers
 * (refactor-3 TEST-GAP-02 / REPL-08):
 *
 *   - ujson_parse        (src/repl/ujson.c)        — full RFC parser used by
 *                                                    introspection payloads
 *   - urepl_ndjson_parse (src/repl/urepl_ndjson.c) — the hand scanner that
 *                                                    actually parses REPL
 *                                                    requests off the wire
 *
 * Both TUs depend only on libc, so the harness links exactly those two
 * sources — no liburbi.a, no VM.  Target property: no crash / no leak on any
 * byte sequence; success paths exercise the free routines so ASan checks
 * ownership too.
 *
 * Build:
 *   make fuzz-json
 *
 * Run:
 *   ./build/host-fuzz/fuzz_json                 # runs until Ctrl-C
 *   ./build/host-fuzz/fuzz_json -runs=20000     # bounded smoke test
 */

#include <stddef.h>
#include <stdint.h>

#include "repl/ujson.h"
#include "repl/urepl_ndjson.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* 1. Full RFC parser. */
    UJsonNode *root = NULL;
    UJsonErr err = UJSON_OK;
    if (ujson_parse((const char *)data, size, &root, &err) == 0) {
        ujson_free_node(root);
    }

    /* 2. NDJSON request hand-scanner. */
    UReplNdjsonReq req;
    if (urepl_ndjson_parse((const char *)data, size, &req) == 0) {
        urepl_ndjson_free_req(&req);
    }
    return 0;
}
