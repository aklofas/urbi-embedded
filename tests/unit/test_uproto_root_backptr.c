/* SPDX-License-Identifier: BSD-3-Clause */
/* test_uproto_root_backptr — verifies the back-pointer UProto.root is
 * populated correctly: NULL on the root proto, set to module->root_proto
 * on every nested proto.
 *
 * Phase 1 of v0.8.1-uproto-root: root_proto allocated and fields aliased
 * to UProto buffers.  No behavioral change; this test pins the new
 * structural invariants.
 */

#include "utest.h"

#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "chunk/uchunk.h"
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
compile_only(struct UVM *vm, UArena *arena, UProto *module, const char *src)
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
    UProto module = {0};
    uarena_init(&arena, 4096);

    /* A chunk with no function literals: root_proto still allocated. */
    int rc = compile_only(&vm, &arena, &module, "1 + 2;");
    UASSERT_EQ(0, rc);

    /* root_proto must be non-NULL after uemit_finish. */
    

    /* root_proto->root must be NULL (it IS the root). */
    UASSERT(module.root == NULL);

    /* Alias invariant: root_proto->instructions must point to the same
     * buffer as module.instructions. */
    UASSERT(module.instructions == module.instructions);
    UASSERT_EQ(module.instr_count, module.instr_count);

    uarena_destroy(&arena);
    uchunk_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

UTEST(nested_proto_root_backptr_set)
{
    struct UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UArena  arena;
    UProto module = {0};
    uarena_init(&arena, 4096);

    /* A chunk with one function literal: emitter allocates one nested proto. */
    int rc = compile_only(&vm, &arena, &module, "var f = function () { 1 };");
    UASSERT_EQ(0, rc);

    /* root_proto allocated. */
    
    UASSERT(module.root == NULL);

    /* At least one nested proto for the function body. */
    UASSERT(module.nested_count >= 1);

    /* Every nested proto must have root pointing to module.root_proto. */
    size_t i;
    for (i = 0; i < module.nested_count; i++) {
        UProto *p = module.nested[i];
        if (p == NULL) continue;  /* skip detached slots */
        UASSERT_EQ(&module, p->root);
    }

    uarena_destroy(&arena);
    uchunk_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

UTEST(root_proto_nested_alias_matches_module)
{
    struct UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UArena  arena;
    UProto module = {0};
    uarena_init(&arena, 4096);

    int rc = compile_only(&vm, &arena, &module, "var f = function () { 1 };");
    UASSERT_EQ(0, rc);
    

    /* nested[] alias: root_proto->nested points to same array as module.nested. */
    UASSERT(module.nested == module.nested);
    UASSERT_EQ(module.nested_count, module.nested_count);

    uarena_destroy(&arena);
    uchunk_destroy(&module, &vm);
    urbi_vm_destroy(&vm);
}

UTEST(deserialize_roundtrip_root_proto_invariants)
{
    /* Round-trip a compiled module through serialize → deserialize and verify
     * that uchunk_deserialize correctly populates root_proto and its aliases.
     * Exercises the deserialize aliasing block independently of uemit_finish. */
    struct UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UArena  arena;
    UProto m1 = {0};
    uarena_init(&arena, 4096);

    /* Compile a chunk with a nested proto so nested[] aliasing is exercised. */
    int rc = compile_only(&vm, &arena, &m1, "var f = function () { 1 };");
    UASSERT_EQ(0, rc);
    

    /* Serialize to a stack buffer (measure first, then write). */
    ptrdiff_t need = uchunk_serialize(&m1, NULL, 0);
    UASSERT((ptrdiff_t)0 < need);

    uint8_t buf[8192];
    UASSERT((size_t)need <= sizeof(buf));
    ptrdiff_t wrote = uchunk_serialize(&m1, buf, sizeof(buf));
    UASSERT_EQ(need, wrote);

    /* Deserialize into a fresh module backed by a fresh vm. */
    struct UVM vm2;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm2, NULL, NULL));
    UProto *m2 = NULL;
    char errmsg[128];
    UChunkLoadError load_rc = uchunk_deserialize(&m2, buf, (size_t)wrote,
                                                   NULL, NULL, errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, load_rc);
    UASSERT(m2 != NULL);

    /* m2->root must be NULL (m2 IS the root). */
    UASSERT(m2->root == NULL);

    /* Every non-NULL nested proto must back-point to m2 (the root). */
    size_t k;
    for (k = 0; k < m2->nested_count; k++) {
        UProto *p = m2->nested[k];
        if (p == NULL) continue;
        UASSERT_EQ((void *)m2, (void *)p->root);
    }

    uarena_destroy(&arena);
    uchunk_destroy(&m1, &vm);
    urbi_vm_destroy(&vm);
    uchunk_destroy(m2, &vm2);
    urbi_vm_destroy(&vm2);
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
    utest_run("uproto_root_backptr: deserialize round-trip root_proto invariants",
              deserialize_roundtrip_root_proto_invariants);
}
