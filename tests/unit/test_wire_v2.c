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
    /* The accept cases above only exercise register 7, still inside the
     * old 4-bit tag-nibble's range (0..15) -- a register truly above
     * fifteen needs a wider proto to hold it. */
    uint32_t d[] = { uinstr_enc_abx(OP_SCOPE_TAG, 20, 2), uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TAG, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    UProto *pd = make_proto(d, 3, 0); pd->max_reg = 40;
    UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)roundtrip(pd, err, sizeof err)); free_proto(pd);
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

UTEST(scope_try_takes_exactly_one_of_catch_or_finally) {
    /* The walker's private RUNNING bit, both kinds at once, and neither
     * are all refused; each single kind loads. */
    static const uint8_t bad[] = { 0x00, USCOPE_F_HAS_CATCH | USCOPE_F_HAS_FINALLY, 0x20, 0x21, 0x80 };
    static const uint8_t good[] = { USCOPE_F_HAS_CATCH, USCOPE_F_HAS_FINALLY };
    char err[128] = {0};
    for (size_t k = 0; k < sizeof bad + sizeof good; k++) {
        uint8_t a = (k < sizeof bad) ? bad[k] : good[k - sizeof bad];
        uint32_t ins[] = {
            uinstr_enc_abx(OP_SCOPE_TRY, a, 2),
            uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TRY, 0, 0),
            uinstr_enc_abc(OP_RET, 0, 0, 0),
        };
        UProto *p = make_proto(ins, 3, 0);
        UASSERT_EQ((int)(k < sizeof bad ? UCHUNK_LOAD_CORRUPT : UCHUNK_LOAD_OK),
                   (int)roundtrip(p, err, sizeof err));
        free_proto(p);
    }
}
UTEST(scope_pop_admits_only_the_defined_bits) {
    static const uint8_t bad[] = {
        USCOPE_POP_TRY | 0x08, USCOPE_POP_TAG | 0x80, 0x00, 0x03,
        USCOPE_POP_TAG | USCOPE_POP_RUN_FINALLY,
    };
    static const uint8_t good[] = { USCOPE_POP_TRY, USCOPE_POP_TAG, USCOPE_POP_TRY | USCOPE_POP_RUN_FINALLY };
    char err[128] = {0};
    for (size_t k = 0; k < sizeof bad + sizeof good; k++) {
        uint8_t a = (k < sizeof bad) ? bad[k] : good[k - sizeof bad];
        uint32_t ins[] = { uinstr_enc_abc(OP_SCOPE_POP, a, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
        UProto *p = make_proto(ins, 2, 0);
        UASSERT_EQ((int)(k < sizeof bad ? UCHUNK_LOAD_CORRUPT : UCHUNK_LOAD_OK),
                   (int)roundtrip(p, err, sizeof err));
        free_proto(p);
    }
}
/* A root of CLOSURE P0, one upvalue prelude word `pre`, RET, over a child
 * with one upvalue; `nsites` names on the root. */
static UChunkLoadError closure_with_prelude(uint32_t pre, uint16_t nsites) {
    uint32_t ins[] = { uinstr_enc_abx(OP_CLOSURE, 0, 0), pre, uinstr_enc_abc(OP_RET, 0, 0, 0) };
    UProto *p = make_proto(ins, 3, nsites);
    UProto *child = uproto_alloc_nested(p, p);
    uint32_t cins[] = { uinstr_enc_abc(OP_LOADNIL, 0, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    child->instructions = malloc(sizeof cins); memcpy(child->instructions, cins, sizeof cins);
    child->instr_count = child->instr_cap = 2; child->line_deltas = calloc(2, 1);
    child->max_reg = 1; child->nupvals = 1;
    char err[128] = {0};
    UChunkLoadError rc = roundtrip(p, err, sizeof err);
    uchunk_destroy(p, NULL);   /* owns the nested child and the site names */
    return rc;
}
UTEST(a_closure_prelude_word_is_not_an_instruction) {
    /* Its opcode byte means nothing: an out-of-range one loads... */
    UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)closure_with_prelude(0xFFu | (1u << 16), 0));
    /* ...an EXTARG one does not widen the RET after it... */
    UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)closure_with_prelude((uint32_t)OP_EXTARG | (1u << 16), 0));
    /* ...and a GETSLOT one does not count as a site-bearing instruction,
     * so it cannot vouch for a site name nothing indexes. */
    UASSERT_EQ((int)UCHUNK_LOAD_CORRUPT,
               (int)closure_with_prelude(uinstr_enc_abc(OP_GETSLOT, 0, 1, 0), 1));
}

/* A root of `ins` over one child with one upvalue, so the word after the
 * root's OP_CLOSURE is a prelude word. */
static UChunkLoadError root_over_one_upvalue_child(const uint32_t *ins, size_t n) {
    UProto *p = make_proto(ins, n, 0);
    UProto *child = uproto_alloc_nested(p, p);
    uint32_t cins[] = { uinstr_enc_abc(OP_LOADNIL, 0, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    child->instructions = malloc(sizeof cins); memcpy(child->instructions, cins, sizeof cins);
    child->instr_count = child->instr_cap = 2; child->line_deltas = calloc(2, 1);
    child->max_reg = 1; child->nupvals = 1;
    char err[160] = {0};
    UChunkLoadError rc = roundtrip(p, err, sizeof err);
    uchunk_destroy(p, NULL);
    return rc;
}
/* The prelude word reads as `MOVE R250, R1`: run as an instruction it
 * would write far past a seven-register frame. */
#define HOSTILE_PRELUDE uinstr_enc_abc(OP_MOVE, 250, 1, 0)

UTEST(a_jump_into_a_closure_prelude_is_rejected) {
    uint32_t ins[] = {
        uinstr_enc_abc(OP_LOADNIL, 0, 0, 0),
        uinstr_enc_abx(OP_JMP, 0, 32768 + 1),            /* forward 1: lands on pc 3 */
        uinstr_enc_abx(OP_CLOSURE, 0, 0),
        HOSTILE_PRELUDE,                                  /* pc 3 */
        uinstr_enc_abc(OP_RET, 0, 0, 0),
    };
    UASSERT_EQ((int)UCHUNK_LOAD_BAD_TARGET, (int)root_over_one_upvalue_child(ins, 5));
    /* The same jump one word further, onto the RET, loads. */
    ins[1] = uinstr_enc_abx(OP_JMP, 0, 32768 + 2);
    UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)root_over_one_upvalue_child(ins, 5));
}
UTEST(a_backward_jump_into_a_closure_prelude_is_rejected) {
    uint32_t ins[] = {
        uinstr_enc_abx(OP_CLOSURE, 0, 0),
        HOSTILE_PRELUDE,                                  /* pc 1 */
        uinstr_enc_abx(OP_JMP, 0, 32768 - 1),            /* backward 1: lands on pc 1 */
        uinstr_enc_abc(OP_RET, 0, 0, 0),
    };
    UASSERT_EQ((int)UCHUNK_LOAD_BAD_TARGET, (int)root_over_one_upvalue_child(ins, 4));
}
UTEST(a_scope_try_handler_into_a_closure_prelude_is_rejected) {
    uint32_t ins[] = {
        uinstr_enc_abx(OP_SCOPE_TRY, USCOPE_F_HAS_CATCH, 2),
        uinstr_enc_abx(OP_CLOSURE, 0, 0),
        HOSTILE_PRELUDE,                                  /* pc 2 */
        uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TRY, 0, 0),
        uinstr_enc_abc(OP_RET, 0, 0, 0),
    };
    UASSERT_EQ((int)UCHUNK_LOAD_BAD_TARGET, (int)root_over_one_upvalue_child(ins, 5));
}
UTEST(a_scope_tag_resume_pc_into_a_closure_prelude_is_rejected) {
    uint32_t ins[] = {
        uinstr_enc_abx(OP_SCOPE_TAG, USCOPE_NO_REG, 2),
        uinstr_enc_abx(OP_CLOSURE, 0, 0),
        HOSTILE_PRELUDE,                                  /* pc 2 */
        uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TAG, 0, 0),
        uinstr_enc_abc(OP_RET, 0, 0, 0),
    };
    UASSERT_EQ((int)UCHUNK_LOAD_BAD_TARGET, (int)root_over_one_upvalue_child(ins, 5));
}
UTEST(an_unwind_to_target_into_a_closure_prelude_is_rejected) {
    uint32_t ins[] = {
        uinstr_enc_abx(OP_UNWIND_TO, 0, 2),
        uinstr_enc_abx(OP_CLOSURE, 0, 0),
        HOSTILE_PRELUDE,                                  /* pc 2 */
        uinstr_enc_abc(OP_RET, 0, 0, 0),
    };
    UASSERT_EQ((int)UCHUNK_LOAD_BAD_TARGET, (int)root_over_one_upvalue_child(ins, 4));
}
UTEST(a_skip_over_a_closure_into_its_prelude_is_rejected) {
    /* TEST steps over exactly one word: over a CLOSURE it lands on the
     * prelude word. */
    uint32_t ins[] = {
        uinstr_enc_abc(OP_LOADNIL, 0, 0, 0),
        uinstr_enc_abc(OP_TEST, 0, 0, 0),
        uinstr_enc_abx(OP_CLOSURE, 0, 0),
        HOSTILE_PRELUDE,                                  /* pc 3 */
        uinstr_enc_abc(OP_RET, 0, 0, 0),
    };
    UASSERT_EQ((int)UCHUNK_LOAD_BAD_TARGET, (int)root_over_one_upvalue_child(ins, 5));
}
UTEST(a_target_one_past_the_end_is_rejected) {
    char err[128] = {0};
    /* A forward jump onto instr_count. */
    uint32_t j[] = { uinstr_enc_abx(OP_JMP, 0, 32768 + 1), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    UProto *pj = make_proto(j, 2, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_JMP_OUT_OF_BOUNDS, (int)roundtrip(pj, err, sizeof err)); free_proto(pj);
    /* A handler pc of instr_count. */
    uint32_t h[] = { uinstr_enc_abx(OP_SCOPE_TRY, USCOPE_F_HAS_CATCH, 3),
                     uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TRY, 0, 0), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    UProto *ph = make_proto(h, 3, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_CORRUPT, (int)roundtrip(ph, err, sizeof err)); free_proto(ph);
    /* An UNWIND_TO target of instr_count. */
    uint32_t u[] = { uinstr_enc_abx(OP_UNWIND_TO, 0, 2), uinstr_enc_abc(OP_RET, 0, 0, 0) };
    UProto *pu = make_proto(u, 2, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_CORRUPT, (int)roundtrip(pu, err, sizeof err)); free_proto(pu);
    /* A skip over the final RET. */
    uint32_t s[] = { uinstr_enc_abc(OP_LOADNIL, 0, 0, 0), uinstr_enc_abc(OP_TEST, 0, 0, 0),
                     uinstr_enc_abc(OP_RET, 0, 0, 0) };
    UProto *ps = make_proto(s, 3, 0);
    UASSERT_EQ((int)UCHUNK_LOAD_BAD_TARGET, (int)roundtrip(ps, err, sizeof err)); free_proto(ps);
}
UTEST(a_prelude_word_with_an_extarg_opcode_byte_does_not_guard_a_target) {
    /* Pass 1 and pass 2 agree a prelude word is not an instruction: an
     * EXTARG opcode byte in one does not make the next instruction an
     * EXTARG operand, so a jump onto that instruction loads. */
    uint32_t ins[] = {
        uinstr_enc_abx(OP_JMP, 0, 32768 + 2),            /* forward 2: lands on pc 3 */
        uinstr_enc_abx(OP_CLOSURE, 0, 0),
        (uint32_t)OP_EXTARG | (1u << 16),                 /* prelude word */
        uinstr_enc_abc(OP_RET, 0, 0, 0),                  /* pc 3 */
    };
    UASSERT_EQ((int)UCHUNK_LOAD_OK, (int)root_over_one_upvalue_child(ins, 4));
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
    utest_run("scope_try_takes_exactly_one_of_catch_or_finally",
              scope_try_takes_exactly_one_of_catch_or_finally);
    utest_run("scope_pop_admits_only_the_defined_bits",
              scope_pop_admits_only_the_defined_bits);
    utest_run("a_closure_prelude_word_is_not_an_instruction",
              a_closure_prelude_word_is_not_an_instruction);
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
    utest_run("a_jump_into_a_closure_prelude_is_rejected",
              a_jump_into_a_closure_prelude_is_rejected);
    utest_run("a_backward_jump_into_a_closure_prelude_is_rejected",
              a_backward_jump_into_a_closure_prelude_is_rejected);
    utest_run("a_scope_try_handler_into_a_closure_prelude_is_rejected",
              a_scope_try_handler_into_a_closure_prelude_is_rejected);
    utest_run("a_scope_tag_resume_pc_into_a_closure_prelude_is_rejected",
              a_scope_tag_resume_pc_into_a_closure_prelude_is_rejected);
    utest_run("an_unwind_to_target_into_a_closure_prelude_is_rejected",
              an_unwind_to_target_into_a_closure_prelude_is_rejected);
    utest_run("a_skip_over_a_closure_into_its_prelude_is_rejected",
              a_skip_over_a_closure_into_its_prelude_is_rejected);
    utest_run("a_target_one_past_the_end_is_rejected",
              a_target_one_past_the_end_is_rejected);
    utest_run("a_prelude_word_with_an_extarg_opcode_byte_does_not_guard_a_target",
              a_prelude_word_with_an_extarg_opcode_byte_does_not_guard_a_target);
}
