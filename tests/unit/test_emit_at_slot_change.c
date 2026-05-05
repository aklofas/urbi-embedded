/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: AST_AT_SLOT_CHANGE emit (T63, spec #4 §4.2).
 *
 * Cases:
 *   1. at (obj.x.changed?) body  → OP_GETSLOT_CHANGE_EVENT precedes
 *                                   OP_AT_EVENT_INSTALL in root chunk
 *   2. at sync (obj.x.changed?) body  → OP_AT_EVENT_SYNC_INSTALL used
 *
 * Source pre-declares `obj` with `var` to satisfy the v1.0 no-globals
 * constraint; the emit arm only needs the receiver to be a resolved local.
 */

#include "utest.h"

#include <string.h>

#include "uarena.h"
#include "uast.h"
#include "uemit.h"
#include "ulex.h"
#include "umodule.h"
#include "uparse.h"
#include "uvm.h"

#define UTEST(name) static void name(void)

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

static UEmitError slot_change_compile(const char *src,
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

static void slot_change_cleanup(UModule *mod, UArena *arena, UVM *vm) {
    umodule_destroy(mod);
    uarena_destroy(arena);
    uvm_destroy(vm);
}

/* Return true if the root chunk contains opcode `op`. */
static bool bytecode_has_op(const UModule *m, UOpcode op) {
    size_t i;
    for (i = 0; i < m->instr_count; i++) {
        if (uinstr_op(m->instructions[i]) == op) return true;
    }
    return false;
}

/* Return index of first instruction with opcode `op`, or -1 if absent. */
static int bytecode_first_op_idx(const UModule *m, UOpcode op) {
    size_t i;
    for (i = 0; i < m->instr_count; i++) {
        if (uinstr_op(m->instructions[i]) == op) return (int)i;
    }
    return -1;
}

/* -----------------------------------------------------------------------
 * Test 1: async at (obj.x.changed?) body  →  GETSLOT_CHANGE_EVENT before
 *         AT_EVENT_INSTALL
 * ----------------------------------------------------------------------- */

UTEST(emit_at_slot_change_emits_getslot_then_at_event_install)
{
    UModule  module = {0};
    UArena   arena;
    UVM      vm;
    UEmitter e;

    UEmitError rc = slot_change_compile(
        "var obj = 0; var body = 0; at (obj.x.changed?) body",
        &module, &arena, &vm, &e);

    UASSERT_EQ(EMIT_OK, rc);
    UASSERT(bytecode_has_op(&module, OP_GETSLOT_CHANGE_EVENT));
    UASSERT(bytecode_has_op(&module, OP_AT_EVENT_INSTALL));

    int gi = bytecode_first_op_idx(&module, OP_GETSLOT_CHANGE_EVENT);
    int ai = bytecode_first_op_idx(&module, OP_AT_EVENT_INSTALL);
    UASSERT(gi >= 0);
    UASSERT(ai >= 0);
    /* GETSLOT_CHANGE_EVENT must appear before AT_EVENT_INSTALL */
    UASSERT(gi < ai);

    slot_change_cleanup(&module, &arena, &vm);
}

/* -----------------------------------------------------------------------
 * Test 2: at sync (obj.x.changed?) body  →  AT_EVENT_SYNC_INSTALL used
 * ----------------------------------------------------------------------- */

UTEST(emit_at_sync_slot_change_uses_sync_install_op)
{
    UModule  module = {0};
    UArena   arena;
    UVM      vm;
    UEmitter e;

    UEmitError rc = slot_change_compile(
        "var obj = 0; var body = 0; at sync (obj.x.changed?) body",
        &module, &arena, &vm, &e);

    UASSERT_EQ(EMIT_OK, rc);
    UASSERT(bytecode_has_op(&module, OP_GETSLOT_CHANGE_EVENT));
    UASSERT(bytecode_has_op(&module, OP_AT_EVENT_SYNC_INSTALL));
    /* The non-sync variant must NOT appear */
    UASSERT(!bytecode_has_op(&module, OP_AT_EVENT_INSTALL));

    slot_change_cleanup(&module, &arena, &vm);
}

/* -----------------------------------------------------------------------
 * Suite entry point
 * ----------------------------------------------------------------------- */

void
test_emit_at_slot_change_suite(void)
{
    printf("test_emit_at_slot_change\n");
    utest_run("emit_at_slot_change_emits_getslot_then_at_event_install",
              emit_at_slot_change_emits_getslot_then_at_event_install);
    utest_run("emit_at_sync_slot_change_uses_sync_install_op",
              emit_at_sync_slot_change_uses_sync_install_op);
}
