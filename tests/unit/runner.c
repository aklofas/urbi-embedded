/* SPDX-License-Identifier: BSD-3-Clause */
/* The frontend unit runner.
 *
 * These suites test the half of the library that turns text into
 * bytecode -- the lexer, the parser and its arena, the emitter, the
 * chunk writer/loader/verifier/disassembler, the varint codec, the
 * intern seam -- plus the public header's value constructors and
 * accessors, which are pure inline code with no runtime behind them.
 *
 * The runtime core has its own runner (tests/rt/), and the language it
 * executes is pinned by the .chk corpus.  Nothing here starts a strand
 * or collects a heap: a suite that wanted to would belong in one of
 * those two instead.
 *
 * Suites take a VM only where the emitter needs one, and only for its
 * string table: uemit_init stores it so identifiers and string literals
 * intern into the same table the runtime will later look them up in. */

#include "utest.h"
#include <stdlib.h>

int utest_checks = 0;
int utest_failures = 0;
int utest_cases_run = 0;
int utest_cases_failed = 0;

void utest_run(const char *name, void (*fn)(void)) {
    int before = utest_failures;
    fn();
    utest_cases_run++;
    if (utest_failures > before) {
        utest_cases_failed++;
        printf("  FAIL %s (%d check(s) failed)\n",
            name, utest_failures - before);
    } else {
        printf("  PASS %s\n", name);
    }
    fflush(stdout);
}

/* One extern + one call per test_*.c file, in file order. */
extern void test_api_version_suite(void);
extern void test_arena_suite(void);
extern void test_arena_reuse_suite(void);
extern void test_ast_alloc_suite(void);
extern void test_ast_string_suite(void);
extern void test_class_decl_parse_suite(void);
extern void test_disasm_suite(void);
extern void test_emit_bytecode_suite(void);
extern void test_intern_suite(void);
extern void test_lex_every_suite(void);
extern void test_lex_float_literals_suite(void);
extern void test_lex_keywords_suite(void);
extern void test_lex_operators_suite(void);
extern void test_lex_string_suite(void);
extern void test_lex_unicode_suite(void);
extern void test_lexer_suite(void);
extern void test_lexer_syncline_suite(void);
extern void test_lshift_parse_suite(void);
extern void test_make_value_suite(void);
extern void test_module_loader_hardening_suite(void);
extern void test_parse_at_event_suite(void);
extern void test_parse_at_slot_change_suite(void);
extern void test_parse_bounds_suite(void);
extern void test_parse_desugar_suite(void);
extern void test_parse_emit_postfix_suite(void);
extern void test_parse_every_suite(void);
extern void test_parse_watcher_suite(void);
extern void test_parser_suite(void);
extern void test_require_suite(void);
extern void test_separators_suite(void);
extern void test_uvalue_layout_suite(void);
extern void test_value_as_suite(void);
extern void test_value_kind_drift_suite(void);
extern void test_value_predicates_suite(void);
extern void test_varint_suite(void);
extern void test_verifier_cross_byte_suite(void);
extern void test_verify_chunk_bounds_suite(void);
extern void test_version_suite(void);
extern void test_wire_v2_suite(void);
extern void test_loader_frame_layout_suite(void);

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);   /* a crashing suite must not lose the log */
    test_api_version_suite();
    test_arena_suite();
    test_arena_reuse_suite();
    test_ast_alloc_suite();
    test_ast_string_suite();
    test_class_decl_parse_suite();
    test_disasm_suite();
    test_emit_bytecode_suite();
    test_intern_suite();
    test_lex_every_suite();
    test_lex_float_literals_suite();
    test_lex_keywords_suite();
    test_lex_operators_suite();
    test_lex_string_suite();
    test_lex_unicode_suite();
    test_lexer_suite();
    test_lexer_syncline_suite();
    test_lshift_parse_suite();
    test_make_value_suite();
    test_module_loader_hardening_suite();
    test_parse_at_event_suite();
    test_parse_at_slot_change_suite();
    test_parse_bounds_suite();
    test_parse_desugar_suite();
    test_parse_emit_postfix_suite();
    test_parse_every_suite();
    test_parse_watcher_suite();
    test_parser_suite();
    test_require_suite();
    test_separators_suite();
    test_uvalue_layout_suite();
    test_value_as_suite();
    test_value_kind_drift_suite();
    test_value_predicates_suite();
    test_varint_suite();
    test_verifier_cross_byte_suite();
    test_verify_chunk_bounds_suite();
    test_version_suite();
    test_wire_v2_suite();
    test_loader_frame_layout_suite();

    printf("\nunit: %d cases, %d failed, %d checks\n",
           utest_cases_run, utest_cases_failed, utest_checks);
    return utest_cases_failed ? 1 : 0;
}
