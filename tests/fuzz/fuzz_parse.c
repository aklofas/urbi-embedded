/* SPDX-License-Identifier: BSD-3-Clause */
/* libFuzzer harness for the parser.
 *
 * Feeds raw bytes through the full lexer + parser pipeline.  Target
 * property: uparse_next_statement returns a finite, non-crashing stream
 * of UAstNodes (possibly including AST_ERROR) until NULL.  UArena lifetime
 * covers the full fuzz iteration; destroyed on exit so ASan can see any
 * leak.
 *
 * Build:
 *   make fuzz-parse
 *
 * Run:
 *   ./build/host-fuzz/fuzz_parse                 # runs until Ctrl-C
 *   ./build/host-fuzz/fuzz_parse -runs=100000    # bounded smoke test
 */

#include <stddef.h>
#include <stdint.h>

#include "uarena.h"
#include "uast.h"
#include "ulex.h"
#include "uparse.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    ULexer lex;
    ulex_init(&lex, (const char *)data, size);

    UArena arena;
    uarena_init(&arena, 4096);

    UParser p;
    uparse_init(&p, &lex, &arena);

    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        /* Forces the compiler to keep the call from being optimized away
         * and forces a branch that reads node->kind, which touches the
         * returned memory and helps libFuzzer credit coverage through
         * the node-producing paths. The (int)kind < 0 test is dead
         * under the current UAstKind enum (values 0..4 = INT, IDENT,
         * UNARY, BINARY, ERROR); if UAstKind ever gains negative
         * sentinels, revisit this guard and the compiler-escape intent
         * together. */
        if ((int)node->kind < 0) break;
    }

    uarena_destroy(&arena);
    return 0;
}
