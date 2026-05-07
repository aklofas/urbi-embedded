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

#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "module/umodule.h"
#include "parse/uparse.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

static UEmitError slot_change_compile(const char *src,
                                      UModule    *mod_out,
                                      UArena     *arena_out,
                                      UVM        *vm_out,
                                      UEmitter   *e_out) {
    urbi_vm_init(vm_out, NULL, NULL);
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
    urbi_vm_destroy(vm);
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
 * Test 3: regression — at (Realm.x.changed?) body  (receiver via global
 *         fallback) must not produce event_reg == body_reg in the
 *         OP_AT_EVENT_INSTALL.  Same desync class as the AST_AT_EVENT
 *         sibling: AST_IDENT global-fallback bumps next_reg only, leaving
 *         freereg stale; emit_function_literal then allocates body_reg
 *         on top of event_reg.
 * ----------------------------------------------------------------------- */

UTEST(emit_at_slot_change_global_receiver_disjoint_regs)
{
    UModule  module = {0};
    UArena   arena;
    UVM      vm;
    UEmitter e;

    UEmitError rc = slot_change_compile(
        "var body = 0; at (Realm.x.changed?) body",
        &module, &arena, &vm, &e);

    UASSERT_EQ(EMIT_OK, rc);

    bool found = false;
    size_t i;
    for (i = 0; i < module.instr_count; i++) {
        uint32_t inst = module.instructions[i];
        if (uinstr_op(inst) == OP_AT_EVENT_INSTALL) {
            uint8_t a = uinstr_a(inst);
            uint8_t b = uinstr_b(inst);
            UASSERT(a != b);  /* event_reg must not collide with body_reg */
            found = true;
            break;
        }
    }
    UASSERT(found);

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
    utest_run("emit_at_slot_change_global_receiver_disjoint_regs",
              emit_at_slot_change_global_receiver_disjoint_regs);
}
