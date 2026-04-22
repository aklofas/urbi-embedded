/* SPDX-License-Identifier: BSD-3-Clause */

#include "utest.h"

#include <stdlib.h>
#include <string.h>

#include "uarena.h"
#include "uemit.h"

#define UTEST(name) static void name(void)

UTEST(uemit_init_zeros_emitter_and_does_not_touch_chunk) {
    Chunk chunk = {0};
    Arena arena;
    uarena_init(&arena, 0);
    Emitter e;
    uemit_init(&e, &chunk, &arena, "repl");

    UASSERT_EQ((uint8_t)0, e.next_reg);
    UASSERT_EQ((uint8_t)0, e.max_reg_seen);
    UASSERT_EQ((uint32_t)0, e.prev_line);
    UASSERT(e.any_stmt_emitted == false);
    UASSERT(e.finished == false);
    UASSERT_EQ(EMIT_OK, e.error);
    UASSERT_EQ((Chunk *)&chunk, e.chunk);
    UASSERT_EQ((Arena *)&arena, e.arena);
    UASSERT_EQ((size_t)0, chunk.instr_count);
    uarena_destroy(&arena);
    uchunk_destroy(&chunk);
}

UTEST(uemit_finish_on_empty_chunk_emits_nothing_and_returns_ok) {
    Chunk chunk = {0};
    Arena arena;
    uarena_init(&arena, 0);
    Emitter e;
    uemit_init(&e, &chunk, &arena, NULL);
    EmitError rc = uemit_finish(&e);
    UASSERT_EQ(EMIT_OK, rc);
    UASSERT(e.finished == true);
    UASSERT_EQ((size_t)0, chunk.instr_count);  /* no RET emitted when no statements */
    UASSERT_EQ((uint8_t)0, chunk.max_reg);
    uarena_destroy(&arena);
    uchunk_destroy(&chunk);
}

UTEST(uemit_finish_is_idempotent_and_statement_after_finish_returns_finished) {
    Chunk chunk = {0};
    Arena arena;
    uarena_init(&arena, 0);
    Emitter e;
    uemit_init(&e, &chunk, &arena, NULL);
    (void)uemit_finish(&e);
    EmitError second = uemit_finish(&e);
    UASSERT_EQ(EMIT_OK, second);              /* finish is idempotent-OK */
    /* Dummy AST_INT to attempt a statement after finish. */
    AstNode dummy = {0};
    dummy.kind = AST_INT;
    dummy.u.i = 7;
    UASSERT_EQ(EMIT_FINISHED, uemit_statement(&e, &dummy));
    uarena_destroy(&arena);
    uchunk_destroy(&chunk);
}

UTEST(uemit_error_name_returns_sensible_strings) {
    UASSERT_EQ(0, strcmp("EMIT_OK",  uemit_error_name(EMIT_OK)));
    UASSERT_EQ(0, strcmp("EMIT_OOM", uemit_error_name(EMIT_OOM)));
    UASSERT(uemit_error_name(EMIT_UNSUPPORTED_AST) != NULL);
}

void test_emit_suite(void);

void test_emit_suite(void) {
    utest_run("uemit_init zeros emitter and does not touch chunk",
              uemit_init_zeros_emitter_and_does_not_touch_chunk);
    utest_run("uemit_finish on empty chunk emits nothing and returns OK",
              uemit_finish_on_empty_chunk_emits_nothing_and_returns_ok);
    utest_run("uemit_finish is idempotent; subsequent statement returns FINISHED",
              uemit_finish_is_idempotent_and_statement_after_finish_returns_finished);
    utest_run("uemit_error_name returns a sensible string",
              uemit_error_name_returns_sensible_strings);
}
