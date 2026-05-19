/* SPDX-License-Identifier: BSD-3-Clause */
/* Verify every UProto in a freshly-created UChunkInstance has its
 * owning_module_instance back-pointer populated.  v0.9.0-repl. */

#include "utest.h"

#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#include "urbi/urbi.h"
#include "chunk/uchunk.h"
#include "object/uchunk_instance.h"
#include "value/uarena.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "emit/uemit.h"
#include "vm/uvm.h"

#define UTEST(name) static void name(void)

/* DFS walk: assert every UProto in the tree has owning_module_instance == mi. */
static void verify_proto_owns(UProto *p, UChunkInstance *mi)
{
    if (p == NULL) return;
    UASSERT(p->owning_module_instance == mi);
    for (size_t i = 0; i < p->nested_count; i++) {
        if (p->nested[i] != NULL) verify_proto_owns(p->nested[i], mi);
    }
}

/* Compile source using the standard emit pipeline. */
static int compile_src(const char *src, UVM *vm, UModule *m, UArena *arena)
{
    ULexer  lex;
    UParser p;
    UEmitter e;
    UAstNode *node;

    ulex_init(&lex, src, strlen(src));
    uemit_init(&e, m, arena, vm, NULL);
    uparse_init(&p, &lex, arena);

    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) return -1;
        if (uemit_statement(&e, node) != EMIT_OK) return -1;
        uarena_reset(arena);
    }
    return (uemit_finish(&e) == EMIT_OK) ? 0 : -1;
}

/* -----------------------------------------------------------------------
 * Test 1: root proto only (flat module, no nested protos)
 * ----------------------------------------------------------------------- */

UTEST(root_proto_owning_mi_populated)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UArena arena;
    UModule mod = {0};
    uarena_init(&arena, 4096);

    int rc = compile_src("1 + 2;", &vm, &mod, &arena);
    UASSERT_EQ(0, rc);
    UASSERT(mod.root_proto != NULL);

    UChunkInstance *mi = urbi_module_instance_create(&vm, &mod);
    UASSERT(mi != NULL);

    UASSERT(mod.root_proto->owning_module_instance == mi);

    uarena_destroy(&arena);
    uchunk_destroy(&mod, &vm);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Test 2: one nested proto (function literal at root level)
 * ----------------------------------------------------------------------- */

UTEST(nested_proto_owning_mi_populated)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UArena arena;
    UModule mod = {0};
    uarena_init(&arena, 4096);

    int rc = compile_src("var f = function () { 1 };", &vm, &mod, &arena);
    UASSERT_EQ(0, rc);
    UASSERT(mod.root_proto != NULL);
    UASSERT(mod.root_proto->nested_count >= 1);

    UChunkInstance *mi = urbi_module_instance_create(&vm, &mod);
    UASSERT(mi != NULL);

    verify_proto_owns(mod.root_proto, mi);

    uarena_destroy(&arena);
    uchunk_destroy(&mod, &vm);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Test 3: recursive (nested function inside nested function)
 * ----------------------------------------------------------------------- */

UTEST(recursive_nested_owning_mi_populated)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UArena arena;
    UModule mod = {0};
    uarena_init(&arena, 4096);

    /* outer() contains inner() — produces at least one level of nesting
     * under root, which is the recursive case the DFS stamper must cover. */
    const char *src = "var outer = function () { var inner = function () { 1 }; inner() };";
    int rc = compile_src(src, &vm, &mod, &arena);
    UASSERT_EQ(0, rc);
    UASSERT(mod.root_proto != NULL);

    UChunkInstance *mi = urbi_module_instance_create(&vm, &mod);
    UASSERT(mi != NULL);

    /* Verify the full tree recursively — root + outer + inner. */
    verify_proto_owns(mod.root_proto, mi);

    uarena_destroy(&arena);
    uchunk_destroy(&mod, &vm);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Test 4: deserialize round-trip — back-pointer survives serialize/load
 * ----------------------------------------------------------------------- */

UTEST(deserialize_roundtrip_owning_mi_populated)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    UArena  arena;
    UModule m1 = {0};
    uarena_init(&arena, 4096);

    int rc = compile_src("var f = function () { 1 };", &vm, &m1, &arena);
    UASSERT_EQ(0, rc);

    /* Serialize. */
    ptrdiff_t need = uchunk_serialize(&m1, NULL, 0);
    UASSERT((ptrdiff_t)0 < need);

    uint8_t buf[8192];
    UASSERT((size_t)need <= sizeof(buf));
    ptrdiff_t wrote = uchunk_serialize(&m1, buf, sizeof(buf));
    UASSERT_EQ(need, wrote);

    /* Deserialize into a fresh module. */
    UModule m2 = {0};
    char errmsg[128];
    UChunkLoadError load_rc = uchunk_deserialize(&m2, buf, (size_t)wrote,
                                                   errmsg, sizeof errmsg);
    UASSERT_EQ(UCHUNK_LOAD_OK, load_rc);
    UASSERT(m2.root_proto != NULL);

    UChunkInstance *mi = urbi_module_instance_create(&vm, &m2);
    UASSERT(mi != NULL);

    verify_proto_owns(m2.root_proto, mi);

    uarena_destroy(&arena);
    uchunk_destroy(&m1, &vm);
    uchunk_destroy(&m2, &vm);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Suite entry
 * ----------------------------------------------------------------------- */

void
test_uproto_owning_mi_suite(void)
{
    utest_run("uproto_owning_mi: root proto back-pointer populated",
              root_proto_owning_mi_populated);
    utest_run("uproto_owning_mi: nested proto back-pointer populated",
              nested_proto_owning_mi_populated);
    utest_run("uproto_owning_mi: recursive nested back-pointers populated",
              recursive_nested_owning_mi_populated);
    utest_run("uproto_owning_mi: deserialize round-trip back-pointer populated",
              deserialize_roundtrip_owning_mi_populated);
}
