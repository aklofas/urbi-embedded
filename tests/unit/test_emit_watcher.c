/* SPDX-License-Identifier: BSD-3-Clause */
/* T33: AST_WATCHER + AST_WAITUNTIL emit cases — unit tests.
 *
 * Verifies that the new emit arms produce the correct install opcodes and
 * that the compile-time side-effect detector fires a warning when the
 * condition expression contains a direct write/assignment.
 *
 * Source-level tests pre-declare variables with `var` to satisfy the v1.0
 * no-globals constraint (bare identifiers produce EMIT_UNRESOLVED_NAME).
 * The warn test constructs the AST manually to inject an AST_ASSIGN cond
 * without going through the parser (assignment is not a sub-expression in
 * the current grammar).
 */

#include "utest.h"

#include <string.h>

#include "value/uarena.h"
#include "uast.h"
#include "uemit.h"
#include "lex/ulex.h"
#include "umodule.h"
#include "uparse.h"
#include "uvm.h"
#include "watcher/uwatcher.h"  /* UWATCHER_AT */

#define UTEST(name) static void name(void)

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/* Compile source; return emit error (EMIT_OK if none).
 * On success, *mod_out is populated with bytecode (caller destroys). */
static UEmitError watcher_compile(const char *src,
                                  UModule    *mod_out,
                                  UArena     *arena_out,
                                  UVM        *vm_out,
                                  UEmitter   *e_out) {
    uvm_init(vm_out, NULL, NULL);
    uarena_init(arena_out, 4096);

    ULexer lex;
    ulex_init(&lex, src, strlen(src));

    uemit_init(e_out, mod_out, arena_out, vm_out, NULL);

    UParser p;
    uparse_init(&p, &lex, arena_out);

    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) break;
        (void)uemit_statement(e_out, node);
        uarena_reset(arena_out);
    }
    return uemit_finish(e_out);
}

static void watcher_cleanup(UModule *mod, UArena *arena, UVM *vm) {
    umodule_destroy(mod);
    uarena_destroy(arena);
    uvm_destroy(vm);
}

/* Return true if any instruction in module (root chunk) has opcode == op. */
static bool bytecode_contains_op(const UModule *m, UOpcode op) {
    size_t i;
    for (i = 0; i < m->instr_count; i++) {
        if (uinstr_op(m->instructions[i]) == op) return true;
    }
    return false;
}

/* -----------------------------------------------------------------------
 * T33 test cases
 * ----------------------------------------------------------------------- */

/* at (x > 5) x  →  bytecode must contain OP_AT_INSTALL (=39).
 * Pre-declare x with `var` to satisfy the v1.0 no-globals constraint. */
UTEST(emit_at_produces_OP_AT_INSTALL) {
    UModule  module = {0};
    UArena   arena;
    UVM      vm;
    UEmitter e;

    UEmitError rc = watcher_compile("var x = 0; at (x > 5) x",
                                    &module, &arena, &vm, &e);
    UASSERT_EQ(EMIT_OK, rc);
    UASSERT(bytecode_contains_op(&module, OP_AT_INSTALL));

    watcher_cleanup(&module, &arena, &vm);
}

/* at sync (cond) body  →  bytecode must contain OP_AT_SYNC_INSTALL (=40). */
UTEST(emit_at_sync_produces_OP_AT_SYNC_INSTALL) {
    UModule  module = {0};
    UArena   arena;
    UVM      vm;
    UEmitter e;

    UEmitError rc = watcher_compile("var cond = 0; var body = 0;"
                                    " at sync (cond) body",
                                    &module, &arena, &vm, &e);
    UASSERT_EQ(EMIT_OK, rc);
    UASSERT(bytecode_contains_op(&module, OP_AT_SYNC_INSTALL));

    watcher_cleanup(&module, &arena, &vm);
}

/* whenever (cond) body  →  bytecode must contain OP_WHENEVER_INSTALL (=41). */
UTEST(emit_whenever_produces_OP_WHENEVER_INSTALL) {
    UModule  module = {0};
    UArena   arena;
    UVM      vm;
    UEmitter e;

    UEmitError rc = watcher_compile("var cond = 0; var body = 0;"
                                    " whenever (cond) body",
                                    &module, &arena, &vm, &e);
    UASSERT_EQ(EMIT_OK, rc);
    UASSERT(bytecode_contains_op(&module, OP_WHENEVER_INSTALL));

    watcher_cleanup(&module, &arena, &vm);
}

/* waituntil (cond)  →  bytecode must contain OP_WAITUNTIL_INSTALL (=42). */
UTEST(emit_waituntil_produces_OP_WAITUNTIL_INSTALL) {
    UModule  module = {0};
    UArena   arena;
    UVM      vm;
    UEmitter e;

    UEmitError rc = watcher_compile("var cond = 0; waituntil (cond)",
                                    &module, &arena, &vm, &e);
    UASSERT_EQ(EMIT_OK, rc);
    UASSERT(bytecode_contains_op(&module, OP_WAITUNTIL_INSTALL));

    watcher_cleanup(&module, &arena, &vm);
}

/* at with assign in cond  →  diag_count >= 1 with side-effect message.
 *
 * Assignment is not a sub-expression in the current grammar, so the AST
 * is constructed manually: an AST_WATCHER node with an AST_ASSIGN cond
 * and an AST_INT body, emitted directly via uemit_statement. */
UTEST(emit_at_with_assign_in_cond_warns) {
    UVM    vm;
    UArena arena;
    UModule module = {0};
    UEmitter e;

    uvm_init(&vm, NULL, NULL);
    uarena_init(&arena, 4096);
    uemit_init(&e, &module, &arena, &vm, NULL);

    /* Build cond: AST_ASSIGN (x = 5) */
    UAstNode int5;
    memset(&int5, 0, sizeof(int5));
    int5.kind  = AST_INT;
    int5.line  = 1;
    int5.col   = 1;
    int5.u.i   = 5;

    UAstNode assign_cond;
    memset(&assign_cond, 0, sizeof(assign_cond));
    assign_cond.kind               = AST_ASSIGN;
    assign_cond.line               = 1;
    assign_cond.col                = 1;
    assign_cond.u.assign.name_start = "x";
    assign_cond.u.assign.name_len   = 1;
    assign_cond.u.assign.value      = &int5;

    /* Build body: AST_INT(0) — minimal side-effect-free expression. */
    UAstNode body_node;
    memset(&body_node, 0, sizeof(body_node));
    body_node.kind = AST_INT;
    body_node.line = 1;
    body_node.col  = 1;
    body_node.u.i  = 0;

    /* Build AST_WATCHER (at mode). */
    UAstNode watcher;
    memset(&watcher, 0, sizeof(watcher));
    watcher.kind             = AST_WATCHER;
    watcher.line             = 1;
    watcher.col              = 1;
    watcher.u.watcher.cond    = &assign_cond;
    watcher.u.watcher.body    = &body_node;
    watcher.u.watcher.onleave = NULL;
    watcher.u.watcher.mode    = UWATCHER_AT;

    (void)uemit_statement(&e, &watcher);
    (void)uemit_finish(&e);

    /* The side-effect detector must have fired at least one diagnostic. */
    UASSERT(e.diag_count >= 1);
    UASSERT(strstr(e.diag_buf[0].message, "side-effect") != NULL ||
            strstr(e.diag_buf[0].message, "feedback loop") != NULL);

    emit_diag_free_all(&e);
    umodule_destroy(&module);
    uarena_destroy(&arena);
    uvm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Suite entry point
 * ----------------------------------------------------------------------- */

void test_emit_watcher_suite(void) {
    utest_run("emit_at_produces_OP_AT_INSTALL",
              emit_at_produces_OP_AT_INSTALL);
    utest_run("emit_at_sync_produces_OP_AT_SYNC_INSTALL",
              emit_at_sync_produces_OP_AT_SYNC_INSTALL);
    utest_run("emit_whenever_produces_OP_WHENEVER_INSTALL",
              emit_whenever_produces_OP_WHENEVER_INSTALL);
    utest_run("emit_waituntil_produces_OP_WAITUNTIL_INSTALL",
              emit_waituntil_produces_OP_WAITUNTIL_INSTALL);
    utest_run("emit_at_with_assign_in_cond_warns",
              emit_at_with_assign_in_cond_warns);
}
