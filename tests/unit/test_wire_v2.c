/* SPDX-License-Identifier: BSD-3-Clause */
/* Wire v2: version byte, site-index widening, EXTARG rules, dry-run writer. */
#include "utest.h"
#include "chunk/uchunk.h"
#include "emit/uemit.h"      /* uchunk_serialize */
#include <string.h>
#include <stdlib.h>

#define UTEST(name) static void name(void)

/* A root proto with `n` instructions, `nsites` site names "s0".."s(n-1)",
 * max_reg 7, no constants, emit-side ownership (constants_owned = false).
 * The allocator is the one uchunk_destroy will free it with. */
static UProto *make_proto(const uint32_t *ins, size_t n, uint16_t nsites) {
    UProto *p = calloc(1, sizeof *p);
    p->alloc_fn = utest_alloc;
    p->instructions = malloc(n * sizeof(uint32_t)); memcpy(p->instructions, ins, n * sizeof(uint32_t));
    p->instr_count = p->instr_cap = n;
    p->line_deltas = calloc(n, 1);
    p->max_reg = 7; p->site_count = nsites;
    if (nsites) {
        p->site_name_strs = calloc(nsites, sizeof(char *));
        for (uint16_t i = 0; i < nsites; i++) { char b[16]; snprintf(b, sizeof b, "s%u", i); p->site_name_strs[i] = strdup(b); }
    }
    p->heap_allocated = true;
    return p;
}
static void free_proto(UProto *p) {
    for (uint16_t i = 0; i < p->site_count; i++) free(p->site_name_strs[i]);
    free(p->site_name_strs); free(p->line_deltas); free(p->instructions); free(p);
}
/* Serialize with the dry-run writer, then load; returns the loader's verdict. */
static UChunkLoadError roundtrip(UProto *p, char *err, size_t cap) {
    ptrdiff_t need = uchunk_serialize(p, NULL, 0);
    UASSERT(need > 24);
    uint8_t *buf = malloc((size_t)need);
    ptrdiff_t wrote = uchunk_serialize(p, buf, (size_t)need);
    UASSERT_EQ(need, wrote);
    UASSERT_EQ(0x20, buf[4]);
    UProto *back = NULL;
    UChunkLoadError rc = uchunk_deserialize(&back, buf, (size_t)need, NULL, NULL, err, cap);
    if (back) uchunk_destroy(back, NULL);
    free(buf);
    return rc;
}

/* 301 GETSLOT R1, R0, site k (k = 0..300), each k >= 256 behind an EXTARG
 * carrying k >> 8, then RET.  Returns the instruction count (347). */
static size_t sites_0_to_300(uint32_t *ins) {
    size_t n = 0;
    for (uint32_t k = 0; k <= 300; k++) {
        if (k >= 256) ins[n++] = uinstr_enc_abx(OP_EXTARG, 0, (uint16_t)(k >> 8));
        ins[n++] = uinstr_enc_abc(OP_GETSLOT, 1, 0, (uint8_t)(k & 0xFFu));
    }
    ins[n++] = uinstr_enc_abc(OP_RET, 1, 0, 0);
    return n;
}

UTEST(a_site_index_above_255_needs_extarg_and_round_trips) {
    uint32_t ins[301 + 45 + 1];
    size_t n = sites_0_to_300(ins);
    UASSERT_EQ(347, n);
    UProto *p = make_proto(ins, n, 301);
    char err[128] = {0};
    UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)roundtrip(p, err, sizeof err));
    free_proto(p);
}
UTEST(a_site_index_at_or_past_site_count_is_rejected) {
    uint32_t ins[301 + 45 + 1];
    size_t n = sites_0_to_300(ins);
    UProto *p = make_proto(ins, n, 300);          /* 300 names: index 300 is out of range */
    char err[128] = {0};
    UASSERT_EQ((int)UCHUNK_LOAD_CORRUPT, (int)roundtrip(p, err, sizeof err));
    free_proto(p);
}
UTEST(extarg_before_a_non_site_opcode_is_rejected) {
    uint32_t ins[] = { uinstr_enc_abx(OP_EXTARG, 0, 1), uinstr_enc_abc(OP_MOVE, 1, 0, 0), uinstr_enc_abc(OP_RET, 1, 0, 0) };
    UProto *p = make_proto(ins, 3, 0);
    char err[128] = {0};
    UASSERT_EQ((int)UCHUNK_LOAD_BAD_EXTARG, (int)roundtrip(p, err, sizeof err));
    free_proto(p);
}
UTEST(extarg_as_the_last_instruction_is_rejected) {
    uint32_t ins[] = { uinstr_enc_abc(OP_LOADNIL, 0, 0, 0), uinstr_enc_abx(OP_EXTARG, 0, 1) };
    UProto *p = make_proto(ins, 2, 0);
    char err[128] = {0};
    UChunkLoadError rc = roundtrip(p, err, sizeof err);
    UASSERT(rc == UCHUNK_LOAD_BAD_EXTARG || rc == UCHUNK_LOAD_CORRUPT);   /* also fails "last is RET" */
    free_proto(p);
}
UTEST(a_jump_may_not_land_between_extarg_and_its_operand) {
    uint32_t ins[] = {
        uinstr_enc_abx(OP_JMP, 0, 32768 + 1),            /* forward 1: lands on pc 2 */
        uinstr_enc_abx(OP_EXTARG, 0, 0),
        uinstr_enc_abc(OP_GETSLOT, 1, 0, 0),
        uinstr_enc_abc(OP_RET, 1, 0, 0),
    };
    UProto *p = make_proto(ins, 4, 1);
    char err[128] = {0};
    UASSERT_EQ((int)UCHUNK_LOAD_BAD_EXTARG, (int)roundtrip(p, err, sizeof err));
    free_proto(p);
}
UTEST(a_handler_pc_may_not_land_after_an_extarg) {
    uint32_t ins[] = {
        uinstr_enc_abx(OP_SCOPE_TRY, USCOPE_F_HAS_CATCH, 2),
        uinstr_enc_abx(OP_EXTARG, 0, 0),
        uinstr_enc_abc(OP_GETSLOT, 1, 0, 0),
        uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TRY, 0, 0),
        uinstr_enc_abc(OP_RET, 1, 0, 0),
    };
    UProto *p = make_proto(ins, 5, 1);
    char err[128] = {0};
    UASSERT_EQ((int)UCHUNK_LOAD_BAD_EXTARG, (int)roundtrip(p, err, sizeof err));
    free_proto(p);
}
UTEST(scope_tag_accepts_a_register_above_fifteen_and_the_no_reg_sentinel) {
    uint32_t a[] = { uinstr_enc_abx(OP_SCOPE_TAG, 7, 2), uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TAG, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    uint32_t b[] = { uinstr_enc_abx(OP_SCOPE_TAG, USCOPE_NO_REG, 2), uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TAG, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    uint32_t c[] = { uinstr_enc_abx(OP_SCOPE_TAG, 8, 2), uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TAG, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    char err[128] = {0};
    UProto *pa = make_proto(a, 3, 0); UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)roundtrip(pa, err, sizeof err)); free_proto(pa);
    UProto *pb = make_proto(b, 3, 0); UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)roundtrip(pb, err, sizeof err)); free_proto(pb);
    UProto *pc = make_proto(c, 3, 0); UASSERT_EQ((int)UCHUNK_LOAD_CORRUPT, (int)roundtrip(pc, err, sizeof err)); free_proto(pc);  /* 8 > max_reg 7 */
}
UTEST(install_needs_three_registers_and_a_mode) {
    uint32_t ok[]  = { uinstr_enc_abc(OP_INSTALL, 5, UINSTALL_AT_COND, UINSTALL_F_HAS_BODY), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    uint32_t bad[] = { uinstr_enc_abc(OP_INSTALL, 6, UINSTALL_AT_COND, UINSTALL_F_HAS_BODY), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    uint32_t bm[]  = { uinstr_enc_abc(OP_INSTALL, 5, 9, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    char err[128] = {0};
    UProto *p1 = make_proto(ok, 2, 0);  UASSERT_EQ((int)UCHUNK_LOAD_OK,      (int)roundtrip(p1, err, sizeof err)); free_proto(p1);
    UProto *p2 = make_proto(bad, 2, 0); UASSERT_EQ((int)UCHUNK_LOAD_CORRUPT, (int)roundtrip(p2, err, sizeof err)); free_proto(p2);
    UProto *p3 = make_proto(bm, 2, 0);  UASSERT_EQ((int)UCHUNK_LOAD_CORRUPT, (int)roundtrip(p3, err, sizeof err)); free_proto(p3);
}
UTEST(join_wait_must_follow_its_fork) {
    uint32_t ok[]  = { uinstr_enc_abc(OP_FORK, 0, 1, UFORK_JOIN), uinstr_enc_abc(OP_JOIN_WAIT, 1, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    uint32_t bad[] = { uinstr_enc_abc(OP_FORK, 0, 1, UFORK_JOIN), uinstr_enc_abc(OP_MOVE, 2, 2, 0), uinstr_enc_abc(OP_JOIN_WAIT, 1, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    char err[128] = {0};
    UProto *p1 = make_proto(ok, 3, 0);  UASSERT_EQ((int)UCHUNK_LOAD_OK,      (int)roundtrip(p1, err, sizeof err)); free_proto(p1);
    UProto *p2 = make_proto(bad, 4, 0); UASSERT_EQ((int)UCHUNK_LOAD_CORRUPT, (int)roundtrip(p2, err, sizeof err)); free_proto(p2);
}
UTEST(the_dry_run_size_equals_the_written_size_for_a_nested_chunk) {
    uint32_t ins[] = { uinstr_enc_abx(OP_CLOSURE, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    UProto *p = make_proto(ins, 2, 0);
    UProto *child = uproto_alloc_nested(p, p);
    uint32_t cins[] = { uinstr_enc_abc(OP_LOADNIL, 0, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    child->instructions = malloc(sizeof cins); memcpy(child->instructions, cins, sizeof cins);
    child->instr_count = child->instr_cap = 2; child->line_deltas = calloc(2, 1); child->max_reg = 1;
    char err[128] = {0};
    UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)roundtrip(p, err, sizeof err));
    uchunk_destroy(p, NULL);   /* owns the nested child now */
}
UTEST(an_old_version_byte_is_rejected) {
    uint32_t ins[] = { uinstr_enc_abc(OP_RET, 0, 0, 0) };
    UProto *p = make_proto(ins, 1, 0);
    ptrdiff_t need = uchunk_serialize(p, NULL, 0);
    uint8_t *buf = malloc((size_t)need); uchunk_serialize(p, buf, (size_t)need);
    buf[4] = 0x1A;
    UProto *back = NULL;
    UASSERT_EQ((int)UCHUNK_LOAD_UNSUPPORTED_VERSION, (int)uchunk_deserialize(&back, buf, (size_t)need, NULL, NULL, NULL, 0));
    free(buf); free_proto(p);
}

void test_wire_v2_suite(void) {
    utest_run("a_site_index_above_255_needs_extarg_and_round_trips",
              a_site_index_above_255_needs_extarg_and_round_trips);
    utest_run("a_site_index_at_or_past_site_count_is_rejected",
              a_site_index_at_or_past_site_count_is_rejected);
    utest_run("extarg_before_a_non_site_opcode_is_rejected",
              extarg_before_a_non_site_opcode_is_rejected);
    utest_run("extarg_as_the_last_instruction_is_rejected",
              extarg_as_the_last_instruction_is_rejected);
    utest_run("a_jump_may_not_land_between_extarg_and_its_operand",
              a_jump_may_not_land_between_extarg_and_its_operand);
    utest_run("a_handler_pc_may_not_land_after_an_extarg",
              a_handler_pc_may_not_land_after_an_extarg);
    utest_run("scope_tag_accepts_a_register_above_fifteen_and_the_no_reg_sentinel",
              scope_tag_accepts_a_register_above_fifteen_and_the_no_reg_sentinel);
    utest_run("install_needs_three_registers_and_a_mode",
              install_needs_three_registers_and_a_mode);
    utest_run("join_wait_must_follow_its_fork",
              join_wait_must_follow_its_fork);
    utest_run("the_dry_run_size_equals_the_written_size_for_a_nested_chunk",
              the_dry_run_size_equals_the_written_size_for_a_nested_chunk);
    utest_run("an_old_version_byte_is_rejected",
              an_old_version_byte_is_rejected);
}
