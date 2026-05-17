/* SPDX-License-Identifier: BSD-3-Clause */
/* test_uproto_root_backptr — verifies the back-pointer UProto.root is
 * populated correctly: NULL on the root proto, set to module->root_proto
 * on every nested proto.
 *
 * Phase 1 of v0.8.1-uproto-root: root_proto allocated and fields aliased
 * to UModule buffers.  No behavioral change; this test pins the new
 * structural invariants.
 */

#include "utest.h"

#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "module/umodule.h"
#include "value/uarena.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "emit/uemit.h"
#include "runtime/umacros.h"

#define UTEST(name) static void name(void)

/* Compile `src` into `module` (caller owns arena + module).
 * Returns 0 on success, non-zero on lex/parse/emit failure.
 * Does NOT run the chunk — caller inspects module fields directly. */
static int
compile_only(struct UVM *vm, UArena *arena, UModule *module, const char *src)
{
    ULexer   lex;
    UEmitter e;
    UParser  p;
    UAstNode *node;

    ulex_init(&lex, src, urbi_strlen(src));
    uemit_init(&e, module, arena, vm, NULL);
    uparse_init(&p, &lex, arena);

    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) return -1;
        if (uemit_statement(&e, node) != EMIT_OK) return -1;
        uarena_reset(arena);
    }
    if (uemit_finish(&e) != EMIT_OK) return -1;
    return 0;
}

/* ---- test cases ---- */

UTEST(root_proto_allocated_after_finish)
{
    struct UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    /* A chunk with no function literals: root_proto still allocated. */
    int rc = compile_only(&vm, &arena, &module, "1 + 2;");
    UASSERT_EQ(0, rc);

    /* root_proto must be non-NULL after uemit_finish. */
    UASSERT(module.root_proto != NULL);

    /* root_proto->root must be NULL (it IS the root). */
    UASSERT(module.root_proto->root == NULL);

    /* Alias invariant: root_proto->instructions must point to the same
     * buffer as module.instructions. */
    UASSERT(module.root_proto->instructions == module.instructions);
    UASSERT_EQ(module.instr_count, module.root_proto->instr_count);

    uarena_destroy(&arena);
    umodule_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

UTEST(nested_proto_root_backptr_set)
{
    struct UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    /* A chunk with one function literal: emitter allocates one nested proto. */
    int rc = compile_only(&vm, &arena, &module, "var f = function () { 1 };");
    UASSERT_EQ(0, rc);

    /* root_proto allocated. */
    UASSERT(module.root_proto != NULL);
    UASSERT(module.root_proto->root == NULL);

    /* At least one nested proto for the function body. */
    UASSERT(module.root_proto->nested_count >= 1);

    /* Every nested proto must have root pointing to module.root_proto. */
    size_t i;
    for (i = 0; i < module.root_proto->nested_count; i++) {
        UProto *p = module.root_proto->nested[i];
        if (p == NULL) continue;  /* skip detached slots */
        UASSERT_EQ(module.root_proto, p->root);
    }

    uarena_destroy(&arena);
    umodule_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

UTEST(root_proto_nested_alias_matches_module)
{
    struct UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UArena  arena;
    UModule module = {0};
    uarena_init(&arena, 4096);

    int rc = compile_only(&vm, &arena, &module, "var f = function () { 1 };");
    UASSERT_EQ(0, rc);
    UASSERT(module.root_proto != NULL);

    /* nested[] alias: root_proto->nested points to same array as module.nested. */
    UASSERT(module.root_proto->nested == module.nested);
    UASSERT_EQ(module.nested_count, module.root_proto->nested_count);

    uarena_destroy(&arena);
    umodule_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

void
test_uproto_root_backptr_suite(void)
{
    utest_run("uproto_root_backptr: root_proto allocated after finish",
              root_proto_allocated_after_finish);
    utest_run("uproto_root_backptr: nested proto back-pointer set",
              nested_proto_root_backptr_set);
    utest_run("uproto_root_backptr: root_proto nested[] aliases module.nested",
              root_proto_nested_alias_matches_module);
}
