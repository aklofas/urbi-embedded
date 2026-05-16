/* SPDX-License-Identifier: BSD-3-Clause */
/* Disassembler coverage for M5 reactive opcodes 38..45.
 *
 * Each test hand-builds a one-instruction UModule, calls uemit_disassemble,
 * and verifies the exact mnemonic and operand formatting that each case
 * emits.  This catches opcodes that fall through to the generic default arm
 * (which misrepresents WAITUNTIL's one-operand encoding, GETSLOT_CHANGE_EVENT's
 * K-prefixed name operand, and LOAD_REALM_GLOBAL's sym() encoding). */

#include "utest.h"

#include <stdlib.h>
#include <string.h>

#include "emit/uemit.h"
#include "module/umodule.h"

#define UTEST(name) static void name(void)

/* Build a minimal one-instruction module; instructions array is heap-
   allocated so umodule_destroy() can free it. */
static UModule make_one_instr_module(uint32_t instr) {
    UModule m = {0};
    m.instructions = (uint32_t *)malloc(sizeof(uint32_t));
    m.instr_cap   = 1;
    m.instr_count = 1;
    m.instructions[0] = instr;
    return m;
}

/* -------------------------------------------------------------------------
 * OP_AT_INSTALL (38): ABC: cond_reg, body_reg, onleave_or_FF
 * Expected: "AT_INSTALL R5, R6, R255"
 * ------------------------------------------------------------------------- */
UTEST(disasm_at_install) {
    UModule m = make_one_instr_module(uinstr_enc_abc(OP_AT_INSTALL, 5U, 6U, 0xFFU));
    char buf[256];
    size_t n = uemit_disassemble(&m, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "AT_INSTALL") != NULL);
    UASSERT(strstr(buf, "R5") != NULL);
    UASSERT(strstr(buf, "R6") != NULL);
    UASSERT(strstr(buf, "R255") != NULL);
    umodule_destroy(&m, NULL);
}

/* -------------------------------------------------------------------------
 * OP_AT_SYNC_INSTALL (39): ABC: cond_reg, body_reg, onleave_or_FF
 * Expected: "AT_SYNC_INSTALL R2, R3, R255"
 * ------------------------------------------------------------------------- */
UTEST(disasm_at_sync_install) {
    UModule m = make_one_instr_module(uinstr_enc_abc(OP_AT_SYNC_INSTALL, 2U, 3U, 0xFFU));
    char buf[256];
    size_t n = uemit_disassemble(&m, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "AT_SYNC_INSTALL") != NULL);
    UASSERT(strstr(buf, "R2") != NULL);
    UASSERT(strstr(buf, "R3") != NULL);
    UASSERT(strstr(buf, "R255") != NULL);
    umodule_destroy(&m, NULL);
}

/* -------------------------------------------------------------------------
 * OP_WHENEVER_INSTALL (40): ABC: cond_reg, body_reg, onleave_or_FF
 * Expected: "WHENEVER_INSTALL R1, R2, R255"
 * ------------------------------------------------------------------------- */
UTEST(disasm_whenever_install) {
    UModule m = make_one_instr_module(uinstr_enc_abc(OP_WHENEVER_INSTALL, 1U, 2U, 0xFFU));
    char buf[256];
    size_t n = uemit_disassemble(&m, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "WHENEVER_INSTALL") != NULL);
    UASSERT(strstr(buf, "R1") != NULL);
    UASSERT(strstr(buf, "R2") != NULL);
    UASSERT(strstr(buf, "R255") != NULL);
    umodule_destroy(&m, NULL);
}

/* -------------------------------------------------------------------------
 * OP_WAITUNTIL_INSTALL (41): ABC: cond_reg, 0, 0  (cond only)
 * Expected: "WAITUNTIL_INSTALL R7" — no spurious R0 operands
 * ------------------------------------------------------------------------- */
UTEST(disasm_waituntil_install) {
    UModule m = make_one_instr_module(uinstr_enc_abc(OP_WAITUNTIL_INSTALL, 7U, 0U, 0U));
    char buf[256];
    size_t n = uemit_disassemble(&m, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "WAITUNTIL_INSTALL") != NULL);
    UASSERT(strstr(buf, "R7") != NULL);
    /* Must NOT show two extra R0 operands — confirm no comma after R7. */
    {
        const char *p = strstr(buf, "R7");
        UASSERT(p != NULL);
        if (p != NULL) UASSERT(p[2] != ',');
    }
    umodule_destroy(&m, NULL);
}

/* -------------------------------------------------------------------------
 * OP_AT_EVENT_INSTALL (42): ABC: event_reg, body_reg, onleave_or_FF
 * Expected: "AT_EVENT_INSTALL R4, R5, R255"
 * ------------------------------------------------------------------------- */
UTEST(disasm_at_event_install) {
    UModule m = make_one_instr_module(uinstr_enc_abc(OP_AT_EVENT_INSTALL, 4U, 5U, 0xFFU));
    char buf[256];
    size_t n = uemit_disassemble(&m, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "AT_EVENT_INSTALL") != NULL);
    UASSERT(strstr(buf, "R4") != NULL);
    UASSERT(strstr(buf, "R5") != NULL);
    UASSERT(strstr(buf, "R255") != NULL);
    umodule_destroy(&m, NULL);
}

/* -------------------------------------------------------------------------
 * OP_AT_EVENT_SYNC_INSTALL (43): ABC: event_reg, body_reg, onleave_or_FF
 * Expected: "AT_EVENT_SYNC_INSTALL R8, R9, R255"
 * ------------------------------------------------------------------------- */
UTEST(disasm_at_event_sync_install) {
    UModule m = make_one_instr_module(uinstr_enc_abc(OP_AT_EVENT_SYNC_INSTALL, 8U, 9U, 0xFFU));
    char buf[256];
    size_t n = uemit_disassemble(&m, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "AT_EVENT_SYNC_INSTALL") != NULL);
    UASSERT(strstr(buf, "R8") != NULL);
    UASSERT(strstr(buf, "R9") != NULL);
    UASSERT(strstr(buf, "R255") != NULL);
    umodule_destroy(&m, NULL);
}

/* -------------------------------------------------------------------------
 * OP_GETSLOT_CHANGE_EVENT (44): ABC: dst_reg, recv_reg, name_sym_id
 * Expected: "GETSLOT_CHANGE_EVENT R0, R1, K3"
 * (C encodes a symbol-table index — displayed as Kn to distinguish from reg)
 * ------------------------------------------------------------------------- */
UTEST(disasm_getslot_change_event) {
    UModule m = make_one_instr_module(uinstr_enc_abc(OP_GETSLOT_CHANGE_EVENT, 0U, 1U, 3U));
    char buf[256];
    size_t n = uemit_disassemble(&m, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "GETSLOT_CHANGE_EVENT") != NULL);
    UASSERT(strstr(buf, "R0") != NULL);
    UASSERT(strstr(buf, "R1") != NULL);
    UASSERT(strstr(buf, "K3") != NULL);
    umodule_destroy(&m, NULL);
}

/* -------------------------------------------------------------------------
 * OP_LOAD_REALM_GLOBAL (45): ABC: dst_reg, sym_id_hi, sym_id_lo
 * Expected: "LOAD_REALM_GLOBAL R2, sym(0,7)"
 * (B and C are a 16-bit symbol id split into hi/lo bytes)
 * ------------------------------------------------------------------------- */
UTEST(disasm_load_realm_global) {
    UModule m = make_one_instr_module(uinstr_enc_abc(OP_LOAD_REALM_GLOBAL, 2U, 0U, 7U));
    char buf[256];
    size_t n = uemit_disassemble(&m, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "LOAD_REALM_GLOBAL") != NULL);
    UASSERT(strstr(buf, "R2") != NULL);
    UASSERT(strstr(buf, "sym(") != NULL);
    umodule_destroy(&m, NULL);
}

/* ===================================================================
 * T122 / COV-004: disasm coverage for the remaining fmt_* opcodes.
 * ===================================================================
 *
 * Each per-opcode formatter at src/emit/uemit_disasm.c is exercised below.
 * Together with the M5 reactive tests above they cover the dispatch
 * table at uemit_disasm.c:387-433.  Pre-T122 src/emit/uemit_disasm.c
 * was 48 % line-covered. */

UTEST(disasm_loadk) {
    UModule m = make_one_instr_module(uinstr_enc_abx(OP_LOADK, 0U, 1U));
    char buf[256];
    UASSERT(uemit_disassemble(&m, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "LOADK") != NULL);
    UASSERT(strstr(buf, "R0") != NULL);
    umodule_destroy(&m, NULL);
}

UTEST(disasm_neg) {
    UModule m = make_one_instr_module(uinstr_enc_abc(OP_NEG, 1U, 2U, 0U));
    char buf[256];
    UASSERT(uemit_disassemble(&m, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "NEG R1, R2") != NULL);
    umodule_destroy(&m, NULL);
}

UTEST(disasm_ret) {
    UModule m = make_one_instr_module(uinstr_enc_abc(OP_RET, 3U, 0U, 0U));
    char buf[256];
    UASSERT(uemit_disassemble(&m, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "RET") != NULL);
    umodule_destroy(&m, NULL);
}

UTEST(disasm_jmp) {
    UModule m = make_one_instr_module(uinstr_enc_abx(OP_JMP, 0U, 0x8005U));
    char buf[256];
    UASSERT(uemit_disassemble(&m, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "JMP") != NULL);
    umodule_destroy(&m, NULL);
}

UTEST(disasm_loadnil_loadbool_loadvoid) {
    UModule m1 = make_one_instr_module(uinstr_enc_abc(OP_LOADNIL, 1U, 0U, 0U));
    char buf[256];
    UASSERT(uemit_disassemble(&m1, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "LOADNIL R1") != NULL);
    umodule_destroy(&m1, NULL);

    UModule m2 = make_one_instr_module(uinstr_enc_abc(OP_LOADBOOL, 2U, 1U, 1U));
    UASSERT(uemit_disassemble(&m2, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "LOADBOOL R2, true (skip)") != NULL);
    umodule_destroy(&m2, NULL);

    UModule m3 = make_one_instr_module(uinstr_enc_abc(OP_LOADVOID, 3U, 0U, 0U));
    UASSERT(uemit_disassemble(&m3, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "LOADVOID R3") != NULL);
    umodule_destroy(&m3, NULL);
}

UTEST(disasm_upval_ops) {
    UModule m1 = make_one_instr_module(uinstr_enc_abc(OP_GETUPVAL, 0U, 1U, 0U));
    char buf[256];
    UASSERT(uemit_disassemble(&m1, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "GETUPVAL R0, U1") != NULL);
    umodule_destroy(&m1, NULL);

    UModule m2 = make_one_instr_module(uinstr_enc_abc(OP_SETUPVAL, 2U, 3U, 0U));
    UASSERT(uemit_disassemble(&m2, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "SETUPVAL U3, R2") != NULL);
    umodule_destroy(&m2, NULL);

    UModule m3 = make_one_instr_module(uinstr_enc_abc(OP_CLOSE, 4U, 0U, 0U));
    UASSERT(uemit_disassemble(&m3, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "CLOSE R4") != NULL);
    umodule_destroy(&m3, NULL);
}

UTEST(disasm_call_test_testset) {
    UModule m1 = make_one_instr_module(uinstr_enc_abc(OP_CALL, 1U, 3U, 2U));
    char buf[256];
    UASSERT(uemit_disassemble(&m1, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "CALL R1, 2 args, 1 results") != NULL);
    umodule_destroy(&m1, NULL);

    UModule m2 = make_one_instr_module(uinstr_enc_abc(OP_TEST, 5U, 0U, 1U));
    UASSERT(uemit_disassemble(&m2, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "TEST R5, skip-if-truthy") != NULL);
    umodule_destroy(&m2, NULL);

    UModule m3 = make_one_instr_module(uinstr_enc_abc(OP_TESTSET, 0U, 1U, 1U));
    UASSERT(uemit_disassemble(&m3, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "TESTSET R0, R1, 1") != NULL);
    umodule_destroy(&m3, NULL);
}

UTEST(disasm_compare_ops) {
    /* OP_EQ A=true: == form */
    UModule m1 = make_one_instr_module(uinstr_enc_abc(OP_EQ, 1U, 2U, 3U));
    char buf[256];
    UASSERT(uemit_disassemble(&m1, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "EQ ==") != NULL);
    umodule_destroy(&m1, NULL);

    /* OP_EQ A=false: != form */
    UModule m1n = make_one_instr_module(uinstr_enc_abc(OP_EQ, 0U, 2U, 3U));
    UASSERT(uemit_disassemble(&m1n, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "EQ !=") != NULL);
    umodule_destroy(&m1n, NULL);

    UModule m2 = make_one_instr_module(uinstr_enc_abc(OP_NEQ, 0U, 4U, 5U));
    UASSERT(uemit_disassemble(&m2, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "NEQ R4, R5") != NULL);
    umodule_destroy(&m2, NULL);

    UModule m3 = make_one_instr_module(uinstr_enc_abc(OP_LT, 0U, 1U, 2U));
    UASSERT(uemit_disassemble(&m3, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "LT R1, R2") != NULL);
    umodule_destroy(&m3, NULL);

    UModule m4 = make_one_instr_module(uinstr_enc_abc(OP_LE, 0U, 1U, 2U));
    UASSERT(uemit_disassemble(&m4, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "LE") != NULL);
    umodule_destroy(&m4, NULL);
}

UTEST(disasm_yield_fork_join) {
    UModule m1 = make_one_instr_module(uinstr_enc_abc(OP_YIELD, 0U, 0U, 0U));
    char buf[256];
    UASSERT(uemit_disassemble(&m1, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "YIELD") != NULL);
    umodule_destroy(&m1, NULL);

    UModule m2 = make_one_instr_module(uinstr_enc_abc(OP_FORK_DETACH, 1U, 0U, 0U));
    UASSERT(uemit_disassemble(&m2, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "FORK_DETACH") != NULL);
    umodule_destroy(&m2, NULL);

    UModule m3 = make_one_instr_module(uinstr_enc_abc(OP_FORK_JOIN, 1U, 2U, 0U));
    UASSERT(uemit_disassemble(&m3, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "FORK_JOIN") != NULL);
    umodule_destroy(&m3, NULL);

    UModule m4 = make_one_instr_module(uinstr_enc_abc(OP_JOIN_WAIT, 1U, 0U, 0U));
    UASSERT(uemit_disassemble(&m4, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "JOIN_WAIT") != NULL);
    umodule_destroy(&m4, NULL);
}

UTEST(disasm_slot_ops) {
    UModule m1 = make_one_instr_module(uinstr_enc_abc(OP_GETSLOT, 0U, 1U, 2U));
    char buf[256];
    UASSERT(uemit_disassemble(&m1, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "GETSLOT") != NULL);
    umodule_destroy(&m1, NULL);

    UModule m2 = make_one_instr_module(uinstr_enc_abc(OP_SETSLOT, 0U, 1U, 2U));
    UASSERT(uemit_disassemble(&m2, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "SETSLOT") != NULL);
    umodule_destroy(&m2, NULL);
}

/* CLOSURE with embedded upvalue prelude: builds a 3-instruction module
 * (OP_CLOSURE + 2 upvalue-prelude pseudo-instrs) plus a UProto stub at
 * module->nested[0] with nupvals=2.  Exercises uemit_disasm.c:73-87
 * (the "upval[%u]: ..." prelude printer).
 *
 * umodule_destroy frees the nested[] array entries via the module's
 * alloc_fn — to keep the alloc/free round-trip safe, the UProto and
 * the nested[] array are allocated with the default allocator (which
 * is just realloc — std-malloc-compatible). */
UTEST(disasm_closure_with_upval_prelude) {
    UModule m = {0};
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

    char buf[512];
    UASSERT(uemit_disassemble(&m, buf, sizeof buf) > 0);
    UASSERT(strstr(buf, "CLOSURE R1, P0") != NULL);
    UASSERT(strstr(buf, "upval[0]: in_stack parent_idx=4") != NULL);
    UASSERT(strstr(buf, "upval[1]: from_upval parent_idx=7") != NULL);

    /* umodule_destroy will free m.nested[0] via the default allocator
     * (which is realloc-based); calloc/realloc are interchangeable here. */
    umodule_destroy(&m, NULL);
}

void test_disasm_suite(void) {
    utest_run("disasm: AT_INSTALL shows mnemonic and registers",
              disasm_at_install);
    utest_run("disasm: AT_SYNC_INSTALL shows mnemonic and registers",
              disasm_at_sync_install);
    utest_run("disasm: WHENEVER_INSTALL shows mnemonic and registers",
              disasm_whenever_install);
    utest_run("disasm: WAITUNTIL_INSTALL shows cond-only (no trailing R0 operands)",
              disasm_waituntil_install);
    utest_run("disasm: AT_EVENT_INSTALL shows mnemonic and registers",
              disasm_at_event_install);
    utest_run("disasm: AT_EVENT_SYNC_INSTALL shows mnemonic and registers",
              disasm_at_event_sync_install);
    utest_run("disasm: GETSLOT_CHANGE_EVENT shows K-prefix for sym operand",
              disasm_getslot_change_event);
    utest_run("disasm: LOAD_REALM_GLOBAL shows sym() encoding",
              disasm_load_realm_global);
    /* T122 / COV-004 — additional fmt_* coverage */
    utest_run("disasm: LOADK", disasm_loadk);
    utest_run("disasm: NEG", disasm_neg);
    utest_run("disasm: RET", disasm_ret);
    utest_run("disasm: JMP", disasm_jmp);
    utest_run("disasm: LOADNIL/LOADBOOL/LOADVOID",
              disasm_loadnil_loadbool_loadvoid);
    utest_run("disasm: GETUPVAL/SETUPVAL/CLOSE", disasm_upval_ops);
    utest_run("disasm: CALL/TEST/TESTSET", disasm_call_test_testset);
    utest_run("disasm: EQ/NEQ/LT/LE", disasm_compare_ops);
    utest_run("disasm: YIELD/FORK_DETACH/FORK_JOIN/JOIN_WAIT",
              disasm_yield_fork_join);
    utest_run("disasm: GETSLOT/SETSLOT", disasm_slot_ops);
    utest_run("disasm: CLOSURE with upval prelude",
              disasm_closure_with_upval_prelude);
}
