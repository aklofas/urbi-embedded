/* SPDX-License-Identifier: BSD-3-Clause */
/* Disassembler coverage for the wire v2 opcode set.
 *
 * Each case hand-builds a small UProto, calls uemit_disassemble, and checks
 * the exact mnemonic and operand formatting the opcode's formatter emits,
 * so an opcode that falls through to the generic "R%u, R%u, R%u" arm (and
 * misrepresents its operands) shows up here. */

#include "utest.h"

#include <stdlib.h>
#include <string.h>

#include "emit/uemit.h"
#include "chunk/uchunk.h"

#define UTEST(name) static void name(void)

static char dis[1024];

/* Disassemble a stack root holding a copy of `ins`; uchunk_destroy frees
   the instruction buffer (heap_allocated = false keeps the struct). */
static const char *dis_n(const uint32_t *ins, size_t n) {
    UProto m = {0};
    m.instructions = (uint32_t *)malloc(n * sizeof(uint32_t));
    memcpy(m.instructions, ins, n * sizeof(uint32_t));
    m.instr_cap = m.instr_count = n;
    UASSERT(uemit_disassemble(&m, dis, sizeof dis) > 0);
    uchunk_destroy(&m, NULL);
    return dis;
}

static const char *dis1(uint32_t ins) { return dis_n(&ins, 1); }

#define HAS(text) UASSERT(strstr(dis, (text)) != NULL)

UTEST(disasm_every_opcode_has_a_name) {
    UASSERT_EQ(1, urbi_emit_disasm_opnames_complete());
}

UTEST(disasm_loadk_neg_ret_jmp) {
    dis1(uinstr_enc_abx(OP_LOADK, 0U, 1U));   HAS("LOADK R0, K1");
    dis1(uinstr_enc_abc(OP_NEG, 1U, 2U, 0U)); HAS("NEG R1, R2");
    dis1(uinstr_enc_abc(OP_RET, 3U, 0U, 0U)); HAS("RET R3");
    dis1(uinstr_enc_abx(OP_JMP, 0U, 0x8005U)); HAS("JMP 5");
    dis1(uinstr_enc_abx(OP_JMP, 0U, 32760U));  HAS("JMP -8");
}

UTEST(disasm_literals) {
    dis1(uinstr_enc_abc(OP_LOADNIL, 1U, 0U, 0U));  HAS("LOADNIL R1");
    dis1(uinstr_enc_abc(OP_LOADBOOL, 2U, 1U, 1U)); HAS("LOADBOOL R2, true (skip)");
    dis1(uinstr_enc_abc(OP_LOADVOID, 3U, 0U, 0U)); HAS("LOADVOID R3");
}

UTEST(disasm_upval_ops) {
    dis1(uinstr_enc_abc(OP_GETUPVAL, 0U, 1U, 0U)); HAS("GETUPVAL R0, U1");
    dis1(uinstr_enc_abc(OP_SETUPVAL, 2U, 3U, 0U)); HAS("SETUPVAL U3, R2");
    dis1(uinstr_enc_abc(OP_CLOSE, 4U, 0U, 0U));    HAS("CLOSE R4");
}

UTEST(disasm_call_test_testset) {
    dis1(uinstr_enc_abc(OP_CALL, 1U, 3U, 2U));    HAS("CALL R1, 2 args, 1 results");
    dis1(uinstr_enc_abc(OP_CALL, 1U, 4U, UCALL_C_METHOD | 2U)); HAS("CALL [method] R1, 2 args, 1 results");
    dis1(uinstr_enc_abc(OP_TEST, 5U, 0U, 1U));    HAS("TEST R5, skip-if-truthy");
    dis1(uinstr_enc_abc(OP_TESTSET, 0U, 1U, 1U)); HAS("TESTSET R0, R1, 1");
}

UTEST(disasm_compare_ops) {
    dis1(uinstr_enc_abc(OP_EQ, 1U, 2U, 3U)); HAS("EQ == R2, R3");
    dis1(uinstr_enc_abc(OP_EQ, 0U, 2U, 3U)); HAS("EQ != R2, R3");
    dis1(uinstr_enc_abc(OP_LT, 1U, 1U, 2U)); HAS("LT R1, R2 (<)");
    dis1(uinstr_enc_abc(OP_LE, 0U, 1U, 2U)); HAS("LE R1, R2 (>)");
}

UTEST(disasm_generic_three_register_format) {
    dis1(uinstr_enc_abc(OP_MOVE, 1U, 2U, 0U));             HAS("MOVE R1, R2, R0");
    dis1(uinstr_enc_abc(OP_ADD, 0U, 1U, 2U));              HAS("ADD R0, R1, R2");
    dis1(uinstr_enc_abc(OP_THROW, 4U, 0U, 0U));            HAS("THROW R4");
    dis1(uinstr_enc_abc(OP_LOAD_CATCH_VALUE, 6U, 0U, 0U)); HAS("LOAD_CATCH_VALUE R6");
    dis1(uinstr_enc_abc(OP_RESUME, 0U, 0U, 0U));           HAS("RESUME");
}

UTEST(disasm_yield_fork_join_wait) {
    dis1(uinstr_enc_abc(OP_YIELD, 0U, 0U, 0U));            HAS("YIELD");
    dis1(uinstr_enc_abc(OP_FORK, 1U, 2U, UFORK_JOIN));     HAS("FORK R1 -> R2 join");
    dis1(uinstr_enc_abc(OP_FORK, 3U, 0xFFU, UFORK_DETACH)); HAS("FORK R3 detach");
    dis1(uinstr_enc_abc(OP_JOIN_WAIT, 2U, 0U, 0U));        HAS("JOIN_WAIT R2");
}

UTEST(disasm_slot_ops_print_their_site) {
    dis1(uinstr_enc_abc(OP_GETSLOT, 0U, 1U, 2U));              HAS("GETSLOT R0, R1, site 2");
    dis1(uinstr_enc_abc(OP_SETSLOT, 0U, 1U, 3U));              HAS("SETSLOT R0, R1, site 3");
    dis1(uinstr_enc_abc(OP_SETSLOT_UPDATE, 4U, 5U, 6U));       HAS("SETSLOT_UPDATE R4, R5, site 6");
    dis1(uinstr_enc_abc(OP_SELF, 2U, 3U, 7U));                 HAS("SELF R2, R3, site 7");
    dis1(uinstr_enc_abc(OP_GETSLOT_CHANGE_EVENT, 0U, 1U, 3U)); HAS("GETSLOT_CHANGE_EVENT R0, R1, site 3");
}

UTEST(disasm_extarg_folds_into_the_site) {
    const uint32_t ins[] = { uinstr_enc_abx(OP_EXTARG, 0U, 1U), uinstr_enc_abc(OP_GETSLOT, 1U, 0U, 44U) };
    dis_n(ins, 2);
    HAS("0000  EXTARG hi=1");
    HAS("0001  GETSLOT R1, R0, site 300");
}

UTEST(disasm_scope_ops) {
    dis1(uinstr_enc_abx(OP_SCOPE_TRY, USCOPE_F_HAS_CATCH, 9U)); HAS("SCOPE_TRY flags=1 -> 9");
    dis1(uinstr_enc_abx(OP_SCOPE_TAG, 17U, 4U));                HAS("SCOPE_TAG R17 -> 4");
    dis1(uinstr_enc_abx(OP_SCOPE_TAG, USCOPE_NO_REG, 4U));      HAS("SCOPE_TAG fresh -> 4");
    dis1(uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TRY, 0U, 0U)); HAS("SCOPE_POP try\n");
    dis1(uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TRY | USCOPE_POP_RUN_FINALLY, 0U, 0U)); HAS("SCOPE_POP try+finally");
    dis1(uinstr_enc_abc(OP_SCOPE_POP, USCOPE_POP_TAG, 0U, 0U)); HAS("SCOPE_POP tag");
    dis1(uinstr_enc_abx(OP_UNWIND_TO, 2U, 12U));                HAS("UNWIND_TO depth=2 -> 12");
}

UTEST(disasm_install_and_globals) {
    dis1(uinstr_enc_abc(OP_INSTALL, 5U, UINSTALL_WHENEVER_EVENT, UINSTALL_F_HAS_BODY | UINSTALL_F_HAS_ALT));
    HAS("INSTALL R5 mode=6 flags=3");
    dis1(uinstr_enc_abc(OP_LOAD_REALM_GLOBAL, 2U, 0U, 7U)); HAS("LOAD_REALM_GLOBAL R2, sym(0,7)");
    dis1(uinstr_enc_abc(OP_LOAD_RECV, 3U, 0U, 0U));         HAS("LOAD_RECV R3");
}

/* CLOSURE with a 2-upvalue child: covers the prelude printer, and the
 * nested proto section that follows the root. */
UTEST(disasm_closure_with_upval_prelude) {
    UProto m = {0};
    m.instructions = (uint32_t *)malloc(3 * sizeof(uint32_t));
    m.instr_cap   = 3;
    m.instr_count = 3;
    m.instructions[0] = uinstr_enc_abx(OP_CLOSURE, 1U, 0U);
    /* prelude entries: B=in_stack flag, C=parent_idx */
    m.instructions[1] = uinstr_enc_abc(0, 0U, 1U, 4U);  /* in_stack=1 idx=4 */
    m.instructions[2] = uinstr_enc_abc(0, 0U, 0U, 7U);  /* in_stack=0 idx=7 */
    /* Stub child UProto at nested[0] with 2 upvals. */
    UProto *child = (UProto *)calloc(1, sizeof(UProto));
    child->nupvals = 2;
    m.nested = (UProto **)malloc(sizeof(UProto *));
    m.nested[0] = child;
    m.nested_count = 1;

    UASSERT(uemit_disassemble(&m, dis, sizeof dis) > 0);
    HAS("CLOSURE R1, P0");
    HAS("upval[0]: in_stack parent_idx=4");
    HAS("upval[1]: from_upval parent_idx=7");
    HAS("; proto P0\n(empty)");

    uchunk_destroy(&m, NULL);
}

/* The whole chunk: the root first with no header, then every nested proto
 * depth-first under a numbered header. */
UTEST(disasm_recurses_into_nested_protos) {
    UProto m = {0};
    m.instructions = (uint32_t *)malloc(sizeof(uint32_t));
    m.instr_cap = m.instr_count = 1;
    m.instructions[0] = uinstr_enc_abc(OP_RET, 0U, 0U, 0U);
    m.nested = (UProto **)malloc(2 * sizeof(UProto *));
    m.nested_count = 2;
    for (int k = 0; k < 2; k++) {
        UProto *c = (UProto *)calloc(1, sizeof(UProto));
        c->instructions = (uint32_t *)malloc(sizeof(uint32_t));
        c->instr_cap = c->instr_count = 1;
        c->instructions[0] = uinstr_enc_abc(OP_LOADNIL, (uint8_t)(k + 5), 0U, 0U);
        m.nested[k] = c;
    }

    UASSERT(uemit_disassemble(&m, dis, sizeof dis) > 0);
    UASSERT(strncmp(dis, "0000  RET R0", 12) == 0);
    const char *p0 = strstr(dis, "; proto P0");
    const char *p1 = strstr(dis, "; proto P1");
    UASSERT(p0 != NULL && p1 != NULL && p0 < p1);
    UASSERT(p0 != NULL && strstr(p0, "LOADNIL R5") != NULL);
    UASSERT(p1 != NULL && strstr(p1, "LOADNIL R6") != NULL);

    uchunk_destroy(&m, NULL);
}

UTEST(disasm_truncates_cleanly) {
    const uint32_t ins[] = { uinstr_enc_abc(OP_LOADNIL, 1U, 0U, 0U), uinstr_enc_abc(OP_RET, 1U, 0U, 0U) };
    UProto m = {0};
    m.instructions = (uint32_t *)malloc(sizeof ins);
    memcpy(m.instructions, ins, sizeof ins);
    m.instr_cap = m.instr_count = 2;
    char small[8];
    size_t n = uemit_disassemble(&m, small, sizeof small);
    UASSERT(n < sizeof small);
    UASSERT_EQ('\0', small[n]);
    uchunk_destroy(&m, NULL);
}

void test_disasm_suite(void) {
    utest_run("disasm: every opcode has a name", disasm_every_opcode_has_a_name);
    utest_run("disasm: LOADK/NEG/RET/JMP", disasm_loadk_neg_ret_jmp);
    utest_run("disasm: LOADNIL/LOADBOOL/LOADVOID", disasm_literals);
    utest_run("disasm: GETUPVAL/SETUPVAL/CLOSE", disasm_upval_ops);
    utest_run("disasm: CALL/TEST/TESTSET", disasm_call_test_testset);
    utest_run("disasm: EQ/LT/LE", disasm_compare_ops);
    utest_run("disasm: generic three-register format", disasm_generic_three_register_format);
    utest_run("disasm: YIELD/FORK/JOIN_WAIT", disasm_yield_fork_join_wait);
    utest_run("disasm: slot opcodes print their site", disasm_slot_ops_print_their_site);
    utest_run("disasm: EXTARG folds into the site", disasm_extarg_folds_into_the_site);
    utest_run("disasm: SCOPE_TRY/SCOPE_TAG/SCOPE_POP/UNWIND_TO", disasm_scope_ops);
    utest_run("disasm: INSTALL/LOAD_REALM_GLOBAL/LOAD_RECV", disasm_install_and_globals);
    utest_run("disasm: CLOSURE with upval prelude", disasm_closure_with_upval_prelude);
    utest_run("disasm: recurses into nested protos", disasm_recurses_into_nested_protos);
    utest_run("disasm: truncates cleanly", disasm_truncates_cleanly);
}
