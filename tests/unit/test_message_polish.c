/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_message_polish.c — mnemonic-leak removal tests.
 *
 * RED before the vm: commit, GREEN after:
 *   vm_format_binop_arith_no_op_mnemonic
 *   vm_format_lt_no_op_mnemonic
 *   vm_format_neg_no_op_mnemonic
 *   repl_eval_slot_missing_no_mnemonic  <- mandatory urbi_repl_eval out_buf test
 *
 * Always GREEN (T13 carry-forward regression guard):
 *   repl_eval_emit_error_uses_stdin_prefix
 */

#include "utest.h"
#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "chunk/uchunk.h"
#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "parse/uparse.h"

#include <stddef.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Compile `src`, run via transient urbi_vm_run, copy vm.last_errmsg into
 * errmsg_out before teardown, and return the run-code. */
static int mp_run_capture(const char *src, char *errmsg_out, size_t cap) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UArena arena;
    uarena_init(&arena, 4096);
    UProto module = {0};
    UEmitter e;
    uemit_init(&e, &module, &arena, &vm, NULL);
    UParser p;
    uparse_init(&p, &lex, &arena);
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) break;
        (void)uemit_statement(&e, node);
        uarena_reset(&arena);
    }
    int rc = 0;
    UValue out = {0};
    if (uemit_finish(&e) == EMIT_OK)
        rc = urbi_vm_run(&vm, NULL, &module, &out);
    /* Copy errmsg before destroy (last_errmsg is in-struct; safe to copy). */
    if (errmsg_out != NULL && cap > 0) {
        size_t n = strlen(vm.last_errmsg);
        if (n >= cap) n = cap - 1;
        memcpy(errmsg_out, vm.last_errmsg, n + 1);
    }
    urbi_vm_destroy(&vm);
    uchunk_destroy(&module, NULL);
    uarena_destroy(&arena);
    return rc;
}

/* --- Tests --- */

/* T13 carry-forward: REPL emit error renders as "<stdin>:line:col: message"
 * in out_buf.  Pins urbi_emit_diag_format_first_error.
 *
 * "ghost = 7" assigns to an undeclared name, triggering EMIT_UNRESOLVED_NAME.
 * The emitter records the error with source position and urbi_repl_eval
 * copies the formatted diagnostic into out_buf. */
UTEST(repl_eval_emit_error_uses_stdin_prefix) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    char out_buf[512];
    out_buf[0] = '\0';
    int rc = urbi_repl_eval(&vm, NULL, "ghost = 7", 9, out_buf, sizeof(out_buf));
    UASSERT(rc != URBI_OK);
    UASSERT(strstr(out_buf, "<stdin>") != NULL);
    UASSERT(strstr(out_buf, "undefined name") != NULL);
    urbi_vm_destroy(&vm);
}

/* Binary arith TypeError must use operator glyph, not opcode mnemonic.
 * "true + 1" is Bool+Integer, which hits the OP_ADD type-error path. */
UTEST(vm_format_binop_arith_no_op_mnemonic) {
    char msg[256];
    int rc = mp_run_capture("true + 1", msg, sizeof(msg));
    UASSERT_EQ(URBI_ERR_UNCAUGHT_THROW, rc);
    UASSERT(strstr(msg, "TypeError") != NULL);
    UASSERT(strstr(msg, "'+'"  ) != NULL);   /* RED before fix */
    UASSERT(strstr(msg, "OP_ADD") == NULL);  /* RED before fix */
}

/* Comparison TypeError must use operator glyph, not opcode mnemonic.
 * "true < 1" is Bool<Integer, which hits the OP_LT type-error path. */
UTEST(vm_format_lt_no_op_mnemonic) {
    char msg[256];
    int rc = mp_run_capture("true < 1", msg, sizeof(msg));
    UASSERT_EQ(URBI_ERR_UNCAUGHT_THROW, rc);
    UASSERT(strstr(msg, "TypeError") != NULL);
    UASSERT(strstr(msg, "'<'"  ) != NULL);   /* RED before fix */
    UASSERT(strstr(msg, "OP_LT") == NULL);   /* RED before fix */
}

/* Unary TypeError must use operator description, not opcode mnemonic.
 * "-true" is NEG on Bool, which hits the OP_NEG type-error path. */
UTEST(vm_format_neg_no_op_mnemonic) {
    char msg[256];
    int rc = mp_run_capture("-true", msg, sizeof(msg));
    UASSERT_EQ(URBI_ERR_UNCAUGHT_THROW, rc);
    UASSERT(strstr(msg, "TypeError") != NULL);
    UASSERT(strstr(msg, "unary '-'") != NULL);  /* RED before fix */
    UASSERT(strstr(msg, "OP_NEG"   ) == NULL);  /* RED before fix */
}

/* Mandatory: urbi_repl_eval out_buf must say "slot access", not "GETSLOT".
 * "asdfjklm" triggers GETSLOT slot-not-found → typed TypeError →
 * capture_uncaught_throw_diag extracts the message into vm->last_errmsg →
 * urbi_repl_eval copies it into out_buf and returns URBI_ERR_STRAND_FATAL. */
UTEST(repl_eval_slot_missing_no_mnemonic) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    char out_buf[512];
    out_buf[0] = '\0';
    int rc = urbi_repl_eval(&vm, NULL, "asdfjklm", 8, out_buf, sizeof(out_buf));
    UASSERT_EQ(URBI_ERR_STRAND_FATAL, rc);                /* typed throw → non-OK */
    UASSERT(strstr(out_buf, "slot access") != NULL);      /* RED before fix */
    UASSERT(strstr(out_buf, "GETSLOT"    ) == NULL);      /* RED before fix */
    urbi_vm_destroy(&vm);
}

void test_message_polish_suite(void) {
    utest_run("repl_eval_emit_error_uses_stdin_prefix", repl_eval_emit_error_uses_stdin_prefix);
    utest_run("vm_format_binop_arith_no_op_mnemonic",   vm_format_binop_arith_no_op_mnemonic);
    utest_run("vm_format_lt_no_op_mnemonic",             vm_format_lt_no_op_mnemonic);
    utest_run("vm_format_neg_no_op_mnemonic",            vm_format_neg_no_op_mnemonic);
    utest_run("repl_eval_slot_missing_no_mnemonic",      repl_eval_slot_missing_no_mnemonic);
}
