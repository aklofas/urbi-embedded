/* SPDX-License-Identifier: BSD-3-Clause */
/* test_emit_string.c — Phase 1: AST_STR emits OP_LOADK with UVAL_STR.
 *
 * Closes the v0.5.6 MOD-008 reservation: the constant-pool kind UVAL_STR
 * is now a writable + readable wire-format kind.  Emitter routes AST_STR
 * through ustr_intern + add_const_str + OP_LOADK. */

#include "utest.h"
#include "value/uarena.h"
#include "value/uintern.h"
#include "emit/uemit.h"
#include "chunk/uchunk.h"
#include "vm/uvm.h"
#include "parse/uast.h"
#include <string.h>

#define UTEST(name) static void name(void)

/* Build AST_STR + drive emit_single_statement.  Constants live in the arena;
 * the AST node references arena-owned bytes via str_lit.bytes. */
static void emit_str_through_pipeline(UVM *vm, UProto *module, UArena *arena,
                                      const char *bytes, int len) {
    char *buf = (char *)uarena_alloc(arena, (size_t)len);
    for (int i = 0; i < len; i++) buf[i] = bytes[i];

    UAstNode n = {0};
    n.kind = AST_STR;
    n.line = 1;
    n.col = 1;
    n.u.str_lit.bytes = buf;
    n.u.str_lit.len = len;

    UEmitter e;
    uemit_init(&e, module, arena, vm, "test");
    UASSERT_EQ(EMIT_OK, uemit_statement(&e, &n));
    UASSERT_EQ(EMIT_OK, uemit_finish(&e));
}

UTEST(emit_string_loadk_with_uval_str_constant) {
    UVM vm;
    UProto module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);

    emit_str_through_pipeline(&vm, &module, &arena, "hello", 5);

    /* Constant pool: one UVAL_STR slot pointing at the interned bytes. */
    UASSERT_EQ((size_t)1, module.const_count);
    UASSERT_EQ((uint8_t)UVAL_STR, module.constants[0].kind);
    const char *interned = (const char *)module.constants[0].v.p;
    UASSERT(interned != NULL);
    UASSERT(memcmp(interned, "hello", 5) == 0);
    UASSERT_EQ('\0', interned[5]);   /* intern table NUL-terminates */

    /* First emitted instruction is OP_LOADK. */
    UASSERT(module.instr_count >= 1);
    UASSERT_EQ((int)OP_LOADK, (int)uinstr_op(module.instructions[0]));

    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
    urbi_vm_destroy(&vm);
}

UTEST(emit_string_dedups_repeated_literal) {
    /* Two equal AST_STR literals share a single UVAL_STR pool slot
     * (intern returns the same pointer; add_const_str dedups by pointer). */
    UVM vm;
    UProto module = {0};
    UArena arena;
    uarena_init(&arena, 0);
    urbi_vm_init(&vm, NULL, NULL);

    char *buf1 = (char *)uarena_alloc(&arena, 3);
    buf1[0] = 'f'; buf1[1] = 'o'; buf1[2] = 'o';
    char *buf2 = (char *)uarena_alloc(&arena, 3);
    buf2[0] = 'f'; buf2[1] = 'o'; buf2[2] = 'o';

    UAstNode a = {0};
    a.kind = AST_STR; a.line = 1; a.col = 1;
    a.u.str_lit.bytes = buf1; a.u.str_lit.len = 3;
    UAstNode b = {0};
    b.kind = AST_STR; b.line = 1; b.col = 7;
    b.u.str_lit.bytes = buf2; b.u.str_lit.len = 3;

    UEmitter e;
    uemit_init(&e, &module, &arena, &vm, "test");
    UASSERT_EQ(EMIT_OK, uemit_statement(&e, &a));
    UASSERT_EQ(EMIT_OK, uemit_statement(&e, &b));
    UASSERT_EQ(EMIT_OK, uemit_finish(&e));

    /* Single pool slot. */
    UASSERT_EQ((size_t)1, module.const_count);
    UASSERT_EQ((uint8_t)UVAL_STR, module.constants[0].kind);

    uarena_destroy(&arena);
    uchunk_destroy(&module, NULL);
    urbi_vm_destroy(&vm);
}

void test_emit_string_suite(void) {
    utest_run("emit_string_loadk_with_uval_str_constant",
              emit_string_loadk_with_uval_str_constant);
    utest_run("emit_string_dedups_repeated_literal",
              emit_string_dedups_repeated_literal);
}
