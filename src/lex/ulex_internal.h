/* SPDX-License-Identifier: BSD-3-Clause */
/* ulex_internal.h — private inter-TU API for the lex subsystem.
 *
 * Public lexer API lives in <lex/ulex.h>.  This header is consumed by
 * src/parse/ for escape-resolution helpers (UTF-8 encoding) that the
 * lexer validates and the parser materializes into the AST string-literal
 * arena buffer.  Test-suite TUs may also include this header to pin
 * helper-level invariants. */

#ifndef ULEX_INTERNAL_H
#define ULEX_INTERNAL_H

#include "ulex.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* urbi_encode_utf8 — emit the UTF-8 byte sequence for a Unicode code point.
 *
 * Pre: cp must be in the range 0..U+10FFFF, and must NOT be in the lone-
 * surrogate range U+D800..U+DFFF.  These preconditions are enforced by the
 * lexer's escape-validation path before this helper is called; the helper
 * itself does no range checking and will produce ill-formed output if the
 * caller violates them.
 *
 * Writes 1-4 bytes to buf and returns the count:
 *   0x000000..0x00007F → 1 byte  (ASCII passthrough)
 *   0x000080..0x0007FF → 2 bytes (110xxxxx 10xxxxxx)
 *   0x000800..0x00FFFF → 3 bytes (1110xxxx 10xxxxxx 10xxxxxx)
 *   0x010000..0x10FFFF → 4 bytes (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
 *
 * Used by the parser's escape-resolution path in src/parse/uparse_expr.c
 * (parse_string_literal) to materialize the bytes for \uXXXX / \u{HHHHHH}
 * escape forms; the lexer itself validates the syntax + range and lets
 * the source bytes flow through as-is in the TOK_STRING view. */
int urbi_encode_utf8(uint32_t cp, unsigned char buf[4]);

#ifdef __cplusplus
}
#endif

#endif
