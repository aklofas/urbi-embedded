/* SPDX-License-Identifier: BSD-3-Clause */
/* libFuzzer harness for the network-facing JSON parser: urepl_ndjson_parse
 * (src/repl/urepl_ndjson.c), the hand scanner that parses REPL requests
 * off the wire.
 *
 * The TU depends only on libc, so the harness links exactly that source:
 * no liburbi.a, no VM.  Target property: no crash and no leak on any byte
 * sequence; the success path exercises the free routine so ASan checks
 * ownership too. */

#include <stddef.h>
#include <stdint.h>

#include "repl/urepl_ndjson.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    UReplNdjsonReq req;
    if (urepl_ndjson_parse((const char *)data, size, &req) == 0) {
        urepl_ndjson_free_req(&req);
    }
    return 0;
}
