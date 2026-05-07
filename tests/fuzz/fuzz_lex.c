/* SPDX-License-Identifier: BSD-3-Clause */
/* libFuzzer harness for the lexer.
 *
 * Feeds raw bytes through ulex_init + ulex_next until TOK_EOF, asserting
 * nothing more than "no crash".  Target property: the lexer produces a
 * finite stream of well-formed UTokens (including TOK_ERROR for malformed
 * input) from any byte sequence, without reading past the end of the
 * buffer and without crashing.  Coverage-guided fuzzing explores the
 * error-recovery paths in particular.
 *
 * Build:
 *   make fuzz-lex
 *
 * Run:
 *   ./build/host-fuzz/fuzz_lex                 # runs until Ctrl-C
 *   ./build/host-fuzz/fuzz_lex -runs=100000    # bounded smoke test
 *   ./build/host-fuzz/fuzz_lex -runs=10000 corpus/  # seed from corpus/
 */

#include <stddef.h>
#include <stdint.h>

#include "lex/ulex.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    ULexer lex;
    ulex_init(&lex, (const char *)data, size);
    for (;;) {
        UToken t = ulex_next(&lex);
        /* The loop only terminates on TOK_EOF. This is safe because the
           lexer contract guarantees every path (including TOK_ERROR)
           advances `cur`, so TOK_EOF is eventually reached. A future
           regression that fails to advance would surface as a libFuzzer
           timeout rather than a crash — keep that invariant in mind
           when touching ulex.c. */
        if (t.type == TOK_EOF) break;
    }
    return 0;
}
