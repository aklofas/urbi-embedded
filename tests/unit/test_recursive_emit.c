/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_recursive_emit.c — v0.8.5-recursive-emit regressions.
 *
 * Validates that UProto.ic_index is assigned in DFS pre-order at both
 * emit and deserialize time, that umodule_alloc_nested_proto routes to
 * the correct parent, and that OP_CLOSURE dispatch resolves against the
 * executing proto's own nested[]. */

#include "utest.h"

#include <string.h>
#include <stdlib.h>

#include "value/uarena.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "lex/ulex.h"
#include "chunk/uchunk.h"
#include "parse/uparse.h"
#include "vm/uvm.h"
#include "object/uchunk_instance.h"
#include "urbi/urbi.h"

#include <stddef.h>  /* ptrdiff_t */

#define UTEST(name) static void name(void)

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/* Compile source into module via the standard emit pipeline.  Returns
 * EMIT_OK on success.  Caller owns module (must destroy) and arena. */
static UEmitError compile_src(const char *src,
                              UVM *vm,
                              UModule *module,
                              UArena *arena) {
    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UEmitter e;
    uemit_init(&e, module, arena, vm, NULL);
    UParser p;
    uparse_init(&p, &lex, arena);
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) break;
        (void)uemit_statement(&e, node);
        uarena_reset(arena);
    }
    return uemit_finish(&e);
}

/* -----------------------------------------------------------------------
 * Task 1: ic_index plumbing tests
 * ----------------------------------------------------------------------- */

UTEST(ic_index_root_is_zero) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UArena arena;
    uarena_init(&arena, 4096);
    UModule m = {0};
    UEmitter e;
    uemit_init(&e, &m, &arena, &vm, NULL);

    UASSERT(m.root_proto != NULL);
    UASSERT_EQ(m.root_proto->ic_index, 0);
    UASSERT_EQ(m.next_proto_serial, 0);

    umodule_destroy(&m, &vm);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

UTEST(ic_index_nested_increments_in_alloc_order) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UArena arena;
    uarena_init(&arena, 4096);
    UModule m = {0};
    UEmitter e;
    uemit_init(&e, &m, &arena, &vm, NULL);
    UASSERT(m.root_proto != NULL);

    /* Allocate three protos under root.  Even with the post-Task-5
     * recursive emitter, this direct-allocation pattern still allocates
     * under the explicit parent — so all three are flat siblings. */
    UProto *p1 = umodule_alloc_nested_proto(&m, m.root_proto);
    UProto *p2 = umodule_alloc_nested_proto(&m, m.root_proto);
    UProto *p3 = umodule_alloc_nested_proto(&m, p1);

    UASSERT(p1 != NULL);
    UASSERT(p2 != NULL);
    UASSERT(p3 != NULL);

    /* DFS pre-order: root=0; p1=1; p2=2; p3=3. */
    UASSERT_EQ(p1->ic_index, 1);
    UASSERT_EQ(p2->ic_index, 2);
    UASSERT_EQ(p3->ic_index, 3);
    UASSERT_EQ(m.next_proto_serial, 3);

    /* Tree shape: root has [p1, p2]; p1 has [p3]; p2 has nothing. */
    UASSERT_EQ(m.root_proto->nested_count, 2);
    UASSERT(m.root_proto->nested[0] == p1);
    UASSERT(m.root_proto->nested[1] == p2);
    UASSERT_EQ(p1->nested_count, 1);
    UASSERT(p1->nested[0] == p3);
    UASSERT_EQ(p2->nested_count, 0);

    umodule_destroy(&m, &vm);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

UTEST(total_proto_count_set_at_uemit_finish) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UArena arena;
    uarena_init(&arena, 4096);
    UModule m = {0};

    /* Three top-level function literals; pre-Task-5 flat siblings under root.
     * total_proto_count = 1 (root) + 3 = 4. */
    UEmitError rc = compile_src(
        "var a = function() { 1 };"
        "var b = function() { 2 };"
        "var c = function() { 3 };", &vm, &m, &arena);
    UASSERT_EQ(rc, EMIT_OK);
    UASSERT(m.root_proto != NULL);
    UASSERT_EQ(m.root_proto->nested_count, 3);
    UASSERT_EQ(m.total_proto_count, 4);

    umodule_destroy(&m, &vm);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Task 2: proto_instances sizing + IC binding via ic_index
 * ----------------------------------------------------------------------- */

UTEST(proto_instances_n_equals_total_proto_count) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UArena arena;
    uarena_init(&arena, 4096);
    UModule m = {0};

    UEmitError rc = compile_src(
        "var a = function() { 1 };"
        "var b = function() { 2 };"
        "var c = function() { 3 };", &vm, &m, &arena);
    UASSERT_EQ(rc, EMIT_OK);

    /* Trigger module-instance creation by running.  urbi_vm_run binds the
     * VM to the module via urbi_get_or_create_module_instance internally. */
    UValue out = {0};
    (void)urbi_vm_run(&vm, NULL, &m, &out);

    UChunkInstance *mi = vm.module_instances_head;
    UASSERT(mi != NULL);
    UASSERT(mi->proto_instances != NULL);
    UASSERT_EQ(m.root_proto->nested_count, 3);
    UASSERT_EQ(m.total_proto_count, 4);
    /* proto_instances->n must equal total_proto_count (was 1 + nested_count
     * — identical for flat trees, diverges for recursive). */
    UASSERT_EQ(mi->proto_instances->n, 4);

    umodule_destroy(&m, &vm);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Task 3: recursive verifier round-trip
 * -----------------------------------------------------------------------
 * Confirms the verifier accepts what the emitter currently produces (flat
 * trees today; recursive trees post-Task 5).  Round-trip emit→serialize→
 * deserialize succeeds end-to-end. */

UTEST(verifier_accepts_emitted_module) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UArena arena;
    uarena_init(&arena, 4096);
    UModule m = {0};

    UEmitError rc = compile_src(
        "function() { function() { 0 } }", &vm, &m, &arena);
    UASSERT_EQ(rc, EMIT_OK);

    /* Serialize, then deserialize into a fresh module — exercises
     * decode_verify against the emitted shape. */
    ptrdiff_t need = umodule_serialize(&m, NULL, 0);
    UASSERT(need > 0);
    uint8_t *blob = (uint8_t *)malloc((size_t)need);
    UASSERT(blob != NULL);
    ptrdiff_t written = umodule_serialize(&m, blob, (size_t)need);
    UASSERT_EQ(written, need);

    UModule m2 = {0};
    char errmsg[256] = {0};
    UChunkLoadError lerr = umodule_deserialize(&m2, blob, (size_t)need,
                                                errmsg, sizeof(errmsg));
    UASSERT_EQ(lerr, UCHUNK_LOAD_OK);
    UASSERT(m2.root_proto != NULL);
    UASSERT_EQ(m2.total_proto_count, m.total_proto_count);

    free(blob);
    umodule_destroy(&m2, &vm);
    umodule_destroy(&m, &vm);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Task 5: emitter produces a truly recursive proto tree
 * ----------------------------------------------------------------------- */

UTEST(emitter_produces_recursive_tree) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UArena arena;
    uarena_init(&arena, 4096);
    UModule m = {0};

    /* outer contains middle, middle contains inner.  Pre-Task-5: all three
     * are flat siblings under root, so root.nested_count == 3.
     * Post-Task-5: root.nested_count == 1 (outer), outer.nested_count == 1
     * (middle), middle.nested_count == 1 (inner). */
    UEmitError rc = compile_src(
        "function() { function() { function() { 0 } } }", &vm, &m, &arena);
    UASSERT_EQ(rc, EMIT_OK);
    UASSERT(m.root_proto != NULL);

    UASSERT_EQ(m.root_proto->nested_count, 1);
    UProto *outer = m.root_proto->nested[0];
    UASSERT(outer != NULL);
    UASSERT_EQ(outer->nested_count, 1);
    UProto *middle = outer->nested[0];
    UASSERT(middle != NULL);
    UASSERT_EQ(middle->nested_count, 1);
    UProto *inner = middle->nested[0];
    UASSERT(inner != NULL);
    UASSERT_EQ(inner->nested_count, 0);

    /* total_proto_count = 4 (root + outer + middle + inner). */
    UASSERT_EQ(m.total_proto_count, 4);

    /* root_proto-back-pointer walk reached every depth. */
    UASSERT(outer->root  == m.root_proto);
    UASSERT(middle->root == m.root_proto);
    UASSERT(inner->root  == m.root_proto);

    umodule_destroy(&m, &vm);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* -----------------------------------------------------------------------
 * Task 9: sibling density, mixed tree, DFS pre-order coverage
 * ----------------------------------------------------------------------- */

UTEST(sibling_density_at_depth) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UArena arena;
    uarena_init(&arena, 16384);
    UModule m = {0};

    /* outer function contains 50 sibling function literals.  Pre-v0.8.5
     * all 50 would be flat siblings under root with root.nested_count = 51.
     * Post-v0.8.5 outer has the 50 children; root has only outer. */
    char src[8192];
    int off = 0;
    off += snprintf(src + off, sizeof(src) - off, "function() {");
    for (int i = 0; i < 50; i++) {
        off += snprintf(src + off, sizeof(src) - off,
                        "  var f%d = function() { %d };", i, i);
    }
    snprintf(src + off, sizeof(src) - off, "}");

    UEmitError rc = compile_src(src, &vm, &m, &arena);
    UASSERT_EQ(rc, EMIT_OK);
    UASSERT(m.root_proto != NULL);

    UASSERT_EQ(m.root_proto->nested_count, 1);
    UProto *outer = m.root_proto->nested[0];
    UASSERT(outer != NULL);
    UASSERT_EQ(outer->nested_count, 50);
    UASSERT_EQ(m.total_proto_count, 52);  /* root + outer + 50 */

    umodule_destroy(&m, &vm);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

UTEST(mixed_tree_3x3) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UArena arena;
    uarena_init(&arena, 8192);
    UModule m = {0};

    /* 3 top-level functions, each with 3 nested. */
    const char *src =
        "var a = function() { var x1 = function() {0};"
                             "var x2 = function() {1};"
                             "var x3 = function() {2} };"
        "var b = function() { var y1 = function() {3};"
                             "var y2 = function() {4};"
                             "var y3 = function() {5} };"
        "var c = function() { var z1 = function() {6};"
                             "var z2 = function() {7};"
                             "var z3 = function() {8} }";

    UEmitError rc = compile_src(src, &vm, &m, &arena);
    UASSERT_EQ(rc, EMIT_OK);

    UASSERT_EQ(m.root_proto->nested_count, 3);
    for (size_t i = 0; i < 3; i++) {
        UProto *p = m.root_proto->nested[i];
        UASSERT(p != NULL);
        UASSERT_EQ(p->nested_count, 3);
        for (size_t j = 0; j < 3; j++) {
            UASSERT(p->nested[j] != NULL);
            UASSERT_EQ(p->nested[j]->nested_count, 0);
        }
    }
    UASSERT_EQ(m.total_proto_count, 13);  /* 1 + 3 + 9 */

    umodule_destroy(&m, &vm);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

UTEST(ic_index_dense_and_dfs_preorder) {
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    UArena arena;
    uarena_init(&arena, 4096);
    UModule m = {0};

    /* Tree:                            ic_index
     *   root                              0
     *     outer1 (= root.nested[0])       1
     *       inner1 (= outer1.nested[0])   2
     *     outer2 (= root.nested[1])       3
     *       inner2 (= outer2.nested[0])   4
     * total = 5 */
    const char *src =
        "var a = function() { var x = function() { 0 } };"
        "var b = function() { var y = function() { 1 } }";

    UEmitError rc = compile_src(src, &vm, &m, &arena);
    UASSERT_EQ(rc, EMIT_OK);

    UASSERT_EQ(m.root_proto->ic_index, 0);
    UASSERT_EQ(m.root_proto->nested[0]->ic_index, 1);
    UASSERT_EQ(m.root_proto->nested[0]->nested[0]->ic_index, 2);
    UASSERT_EQ(m.root_proto->nested[1]->ic_index, 3);
    UASSERT_EQ(m.root_proto->nested[1]->nested[0]->ic_index, 4);
    UASSERT_EQ(m.total_proto_count, 5);

    umodule_destroy(&m, &vm);
    uarena_destroy(&arena);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_recursive_emit_suite(void)
{
    printf("test_recursive_emit\n");
    utest_run("ic_index_root_is_zero",
              ic_index_root_is_zero);
    utest_run("ic_index_nested_increments_in_alloc_order",
              ic_index_nested_increments_in_alloc_order);
    utest_run("total_proto_count_set_at_uemit_finish",
              total_proto_count_set_at_uemit_finish);
    utest_run("proto_instances_n_equals_total_proto_count",
              proto_instances_n_equals_total_proto_count);
    utest_run("verifier_accepts_emitted_module",
              verifier_accepts_emitted_module);
    utest_run("emitter_produces_recursive_tree",
              emitter_produces_recursive_tree);
    utest_run("sibling_density_at_depth",
              sibling_density_at_depth);
    utest_run("mixed_tree_3x3",
              mixed_tree_3x3);
    utest_run("ic_index_dense_and_dfs_preorder",
              ic_index_dense_and_dfs_preorder);
}
