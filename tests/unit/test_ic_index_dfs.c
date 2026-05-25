/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests for the ic_index DFS pre-order verifier (bytecode F3, W8).
 *
 * Tests call uchunk_verify_ic_index() directly on hand-constructed UProto
 * trees, allowing mismatch injection that cannot be triggered through the
 * wire-format path (since the deserializer assigns ic_index correctly by
 * construction).
 *
 * Also includes a round-trip test: deserialize a hand-crafted chunk with
 * nested protos and confirm the ic_index values match DFS visit order. */

#include "utest.h"
#include "chunk/uchunk.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* =========================================================================
 * Helpers
 * ========================================================================= */

/* Build a standalone root UProto (stack-allocated) suitable for use with
 * uproto_alloc_nested.  The root is zero-initialised; ic_index = 0 and
 * next_proto_serial = 0 are established by the zero-init.
 * alloc_fn == NULL triggers stdlib realloc on hosted builds. */
static UProto make_root(void) {
    UProto root = {0};
    return root;
}

/* =========================================================================
 * Test 1: root-only tree passes
 * ========================================================================= */

UTEST(ic_index_root_only_passes)
{
    UProto root = make_root();
    char errmsg[256];
    UChunkLoadError rc = uchunk_verify_ic_index(&root, errmsg, sizeof(errmsg));
    UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)rc);
    /* nested[] was never allocated; nothing to free. */
}

/* =========================================================================
 * Test 2: NULL root returns UCHUNK_LOAD_INVALID_ARG
 * ========================================================================= */

UTEST(ic_index_null_root_invalid_arg)
{
    UChunkLoadError rc = uchunk_verify_ic_index(NULL, NULL, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_INVALID_ARG, (int)rc);
}

/* =========================================================================
 * Test 3: two flat children, correct ic_index values (0, 1, 2)
 * ========================================================================= */

UTEST(ic_index_two_children_correct)
{
    UProto root = make_root();
    /* uproto_alloc_nested assigns ic_index = 1 and 2 via ++next_proto_serial. */
    UProto *c0 = uproto_alloc_nested(&root, &root);
    UASSERT(c0 != NULL);
    UProto *c1 = uproto_alloc_nested(&root, &root);
    UASSERT(c1 != NULL);

    UASSERT_EQ(0U, (unsigned)root.ic_index);
    UASSERT_EQ(1U, (unsigned)c0->ic_index);
    UASSERT_EQ(2U, (unsigned)c1->ic_index);

    char errmsg[256];
    UChunkLoadError rc = uchunk_verify_ic_index(&root, errmsg, sizeof(errmsg));
    UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)rc);

    uchunk_destroy(&root, NULL);
}

/* =========================================================================
 * Test 4: swapped ic_index on two sibling children -> mismatch
 * ========================================================================= */

UTEST(ic_index_swapped_siblings_rejected)
{
    UProto root = make_root();
    UProto *c0 = uproto_alloc_nested(&root, &root);
    UASSERT(c0 != NULL);
    UProto *c1 = uproto_alloc_nested(&root, &root);
    UASSERT(c1 != NULL);

    /* DFS pre-order: root(0) -> c0(1) -> c1(2).
     * Swap: c0 now claims 2 and c1 claims 1.
     * Verifier visits c0 first (DFS), expects 1, sees 2 -> mismatch. */
    c0->ic_index = 2;
    c1->ic_index = 1;

    char errmsg[256];
    UChunkLoadError rc = uchunk_verify_ic_index(&root, errmsg, sizeof(errmsg));
    UASSERT_EQ((int)UCHUNK_LOAD_IC_INDEX_MISMATCH, (int)rc);
    UASSERT(strlen(errmsg) > 0);

    /* Restore before destroy so uchunk_destroy's integrity holds. */
    c0->ic_index = 1;
    c1->ic_index = 2;
    uchunk_destroy(&root, NULL);
}

/* =========================================================================
 * Test 5: wrong ic_index on root -> mismatch
 * ========================================================================= */

UTEST(ic_index_root_nonzero_rejected)
{
    UProto root = make_root();
    root.ic_index = 1;  /* root must be 0 */

    char errmsg[256];
    UChunkLoadError rc = uchunk_verify_ic_index(&root, errmsg, sizeof(errmsg));
    UASSERT_EQ((int)UCHUNK_LOAD_IC_INDEX_MISMATCH, (int)rc);
    UASSERT(strlen(errmsg) > 0);
}

/* =========================================================================
 * Test 6: deeply nested (grandchild), correct DFS ordering
 *
 *   root(0)
 *     child(1)
 *       grandchild(2)
 * ========================================================================= */

UTEST(ic_index_deep_nesting_correct)
{
    UProto root = make_root();
    UProto *child = uproto_alloc_nested(&root, &root);
    UASSERT(child != NULL);
    /* Grandchild nested under child; DFS pre-order gives it serial 2. */
    UProto *grand = uproto_alloc_nested(&root, child);
    UASSERT(grand != NULL);

    UASSERT_EQ(0U, (unsigned)root.ic_index);
    UASSERT_EQ(1U, (unsigned)child->ic_index);
    UASSERT_EQ(2U, (unsigned)grand->ic_index);

    char errmsg[256];
    UChunkLoadError rc = uchunk_verify_ic_index(&root, errmsg, sizeof(errmsg));
    UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)rc);

    uchunk_destroy(&root, NULL);
}

/* =========================================================================
 * Test 7: grandchild with wrong ic_index -> mismatch
 *
 *   root(0) -> child(1) -> grandchild(WRONG=99)
 * ========================================================================= */

UTEST(ic_index_grandchild_wrong_rejected)
{
    UProto root = make_root();
    UProto *child = uproto_alloc_nested(&root, &root);
    UASSERT(child != NULL);
    UProto *grand = uproto_alloc_nested(&root, child);
    UASSERT(grand != NULL);

    grand->ic_index = 99;  /* should be 2 */

    char errmsg[256];
    UChunkLoadError rc = uchunk_verify_ic_index(&root, errmsg, sizeof(errmsg));
    UASSERT_EQ((int)UCHUNK_LOAD_IC_INDEX_MISMATCH, (int)rc);

    grand->ic_index = 2;  /* restore before destroy */
    uchunk_destroy(&root, NULL);
}

/* =========================================================================
 * Test 8: uchunk_deserialize of a well-formed chunk with two nested protos
 * passes the ic_index verifier (regression guard for Pass 3 in the loader).
 * ========================================================================= */

static void dfs_build_good_header(uint8_t hdr[24]) {
    size_t i;
    for (i = 0; i < 24; i++) hdr[i] = 0;
    hdr[0] = 'U'; hdr[1] = 'R'; hdr[2] = 'B'; hdr[3] = 'I';
    hdr[4] = (uint8_t)URBI_BYTECODE_VERSION_BYTE;
    hdr[5] = 0x00;
    hdr[6]  = 0x19; hdr[7]  = 0x93;
    hdr[8]  = '\r'; hdr[9]  = '\n';
    hdr[10] = 0x1A; hdr[11] = '\n';
    hdr[12] = 8;   /* int_width  = i64 */
    hdr[13] = 8;   /* float_type = f64 */
    hdr[14] = 4;   /* instr_width = uint32 */
    hdr[15] = 0;   /* endianness = little */
}

static size_t dfs_put_varint(uint8_t *buf, size_t offset, uint64_t v) {
    while (v >= 0x80U) {
        buf[offset++] = (uint8_t)((v & 0x7FU) | 0x80U);
        v >>= 7;
    }
    buf[offset++] = (uint8_t)v;
    return offset;
}

static size_t dfs_put_instr(uint8_t *buf, size_t offset, uint32_t instr) {
    buf[offset++] = (uint8_t)(instr & 0xFFU);
    buf[offset++] = (uint8_t)((instr >> 8)  & 0xFFU);
    buf[offset++] = (uint8_t)((instr >> 16) & 0xFFU);
    buf[offset++] = (uint8_t)((instr >> 24) & 0xFFU);
    return offset;
}

/* Emit a proto block with one OP_RET instruction and nested_count children. */
static size_t dfs_emit_proto(uint8_t *buf, size_t off, size_t nested_count) {
    buf[off++] = 0; /* max_reg */
    buf[off++] = 0; /* nupvals */
    buf[off++] = 0; /* nparams */
    off = dfs_put_varint(buf, off, 0); /* n_const */
    off = dfs_put_varint(buf, off, 1); /* n_instr */
    while ((off & 3U) != 0U) buf[off++] = 0;
    off = dfs_put_instr(buf, off, (uint32_t)OP_RET);
    off = dfs_put_varint(buf, off, 1);  /* n_deltas = 1 */
    buf[off++] = 0;                     /* delta[0] = 0 */
    off = dfs_put_varint(buf, off, 0);  /* n_abs_lines */
    off = dfs_put_varint(buf, off, 0);  /* ic_count */
    off = dfs_put_varint(buf, off, nested_count);
    return off;
}

UTEST(deserialize_with_nested_protos_passes_ic_check)
{
    /* Build: root(2 children) -> child0(none), child1(none).
     * DFS pre-order: root=0, child0=1, child1=2. */
    uint8_t buf[512];
    dfs_build_good_header(buf);
    size_t off = 24;
    off = dfs_put_varint(buf, off, 0); /* source_name_len = 0 */
    /* Root: 2 nested children. */
    off = dfs_emit_proto(buf, off, 2);
    /* Child 0: no nested children. */
    off = dfs_emit_proto(buf, off, 0);
    /* Child 1: no nested children. */
    off = dfs_emit_proto(buf, off, 0);

    UProto *m = NULL;
    char errmsg[256];
    UChunkLoadError rc = uchunk_deserialize(&m, buf, off, NULL, NULL,
                                           errmsg, sizeof(errmsg));
    UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)rc);
    UASSERT(m != NULL);

    /* Confirm ic_index values match DFS pre-order. */
    UASSERT_EQ(0U, (unsigned)m->ic_index);
    UASSERT_EQ(2U, (unsigned)m->nested_count);
    UASSERT(m->nested[0] != NULL);
    UASSERT(m->nested[1] != NULL);
    UASSERT_EQ(1U, (unsigned)m->nested[0]->ic_index);
    UASSERT_EQ(2U, (unsigned)m->nested[1]->ic_index);

    uchunk_destroy(m, NULL);
}

/* =========================================================================
 * Test 9: error code name lookup for UCHUNK_LOAD_IC_INDEX_MISMATCH
 * ========================================================================= */

UTEST(ic_index_error_code_has_name)
{
    const char *name = uchunk_load_error_name(UCHUNK_LOAD_IC_INDEX_MISMATCH);
    UASSERT(name != NULL);
    UASSERT(strcmp(name, "UCHUNK_LOAD_IC_INDEX_MISMATCH") == 0);
    UASSERT(strcmp(name, "UCHUNK_LOAD_UNKNOWN") != 0);
}

/* =========================================================================
 * Suite registration
 * ========================================================================= */

void test_ic_index_dfs_suite(void) {
    utest_run("ic_index_root_only_passes",
              ic_index_root_only_passes);
    utest_run("ic_index_null_root_invalid_arg",
              ic_index_null_root_invalid_arg);
    utest_run("ic_index_two_children_correct",
              ic_index_two_children_correct);
    utest_run("ic_index_swapped_siblings_rejected",
              ic_index_swapped_siblings_rejected);
    utest_run("ic_index_root_nonzero_rejected",
              ic_index_root_nonzero_rejected);
    utest_run("ic_index_deep_nesting_correct",
              ic_index_deep_nesting_correct);
    utest_run("ic_index_grandchild_wrong_rejected",
              ic_index_grandchild_wrong_rejected);
    utest_run("deserialize_with_nested_protos_passes_ic_check",
              deserialize_with_nested_protos_passes_ic_check);
    utest_run("ic_index_error_code_has_name",
              ic_index_error_code_has_name);
}
