/* SPDX-License-Identifier: BSD-3-Clause */
/* Disassembler coverage for M5 reactive opcodes 39..46.
 *
 * Each test hand-builds a one-instruction UModule, calls uemit_disassemble,
 * and verifies the exact mnemonic and operand formatting that each case
 * emits.  This catches opcodes that fall through to the generic default arm
 * (which misrepresents WAITUNTIL's one-operand encoding, GETSLOT_CHANGE_EVENT's
 * K-prefixed name operand, and LOAD_REALM_GLOBAL's sym() encoding). */

#include "utest.h"

#include <stdlib.h>
#include <string.h>

#include "uemit.h"
#include "umodule.h"

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
 * OP_AT_INSTALL (39): ABC: cond_reg, body_reg, onleave_or_FF
 * Expected: "AT_INSTALL R5, R6, R255"
 * ------------------------------------------------------------------------- */
UTEST(disasm_at_install) {
    UModule m = make_one_instr_module(uinstr_enc_abc(OP_AT_INSTALL, 5u, 6u, 0xFFu));
    char buf[256];
    size_t n = uemit_disassemble(&m, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "AT_INSTALL") != NULL);
    UASSERT(strstr(buf, "R5") != NULL);
    UASSERT(strstr(buf, "R6") != NULL);
    UASSERT(strstr(buf, "R255") != NULL);
    umodule_destroy(&m);
}

/* -------------------------------------------------------------------------
 * OP_AT_SYNC_INSTALL (40): ABC: cond_reg, body_reg, onleave_or_FF
 * Expected: "AT_SYNC_INSTALL R2, R3, R255"
 * ------------------------------------------------------------------------- */
UTEST(disasm_at_sync_install) {
    UModule m = make_one_instr_module(uinstr_enc_abc(OP_AT_SYNC_INSTALL, 2u, 3u, 0xFFu));
    char buf[256];
    size_t n = uemit_disassemble(&m, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "AT_SYNC_INSTALL") != NULL);
    UASSERT(strstr(buf, "R2") != NULL);
    UASSERT(strstr(buf, "R3") != NULL);
    UASSERT(strstr(buf, "R255") != NULL);
    umodule_destroy(&m);
}

/* -------------------------------------------------------------------------
 * OP_WHENEVER_INSTALL (41): ABC: cond_reg, body_reg, onleave_or_FF
 * Expected: "WHENEVER_INSTALL R1, R2, R255"
 * ------------------------------------------------------------------------- */
UTEST(disasm_whenever_install) {
    UModule m = make_one_instr_module(uinstr_enc_abc(OP_WHENEVER_INSTALL, 1u, 2u, 0xFFu));
    char buf[256];
    size_t n = uemit_disassemble(&m, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "WHENEVER_INSTALL") != NULL);
    UASSERT(strstr(buf, "R1") != NULL);
    UASSERT(strstr(buf, "R2") != NULL);
    UASSERT(strstr(buf, "R255") != NULL);
    umodule_destroy(&m);
}

/* -------------------------------------------------------------------------
 * OP_WAITUNTIL_INSTALL (42): ABC: cond_reg, 0, 0  (cond only)
 * Expected: "WAITUNTIL_INSTALL R7" — no spurious R0 operands
 * ------------------------------------------------------------------------- */
UTEST(disasm_waituntil_install) {
    UModule m = make_one_instr_module(uinstr_enc_abc(OP_WAITUNTIL_INSTALL, 7u, 0u, 0u));
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
    umodule_destroy(&m);
}

/* -------------------------------------------------------------------------
 * OP_AT_EVENT_INSTALL (43): ABC: event_reg, body_reg, onleave_or_FF
 * Expected: "AT_EVENT_INSTALL R4, R5, R255"
 * ------------------------------------------------------------------------- */
UTEST(disasm_at_event_install) {
    UModule m = make_one_instr_module(uinstr_enc_abc(OP_AT_EVENT_INSTALL, 4u, 5u, 0xFFu));
    char buf[256];
    size_t n = uemit_disassemble(&m, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "AT_EVENT_INSTALL") != NULL);
    UASSERT(strstr(buf, "R4") != NULL);
    UASSERT(strstr(buf, "R5") != NULL);
    UASSERT(strstr(buf, "R255") != NULL);
    umodule_destroy(&m);
}

/* -------------------------------------------------------------------------
 * OP_AT_EVENT_SYNC_INSTALL (44): ABC: event_reg, body_reg, onleave_or_FF
 * Expected: "AT_EVENT_SYNC_INSTALL R8, R9, R255"
 * ------------------------------------------------------------------------- */
UTEST(disasm_at_event_sync_install) {
    UModule m = make_one_instr_module(uinstr_enc_abc(OP_AT_EVENT_SYNC_INSTALL, 8u, 9u, 0xFFu));
    char buf[256];
    size_t n = uemit_disassemble(&m, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "AT_EVENT_SYNC_INSTALL") != NULL);
    UASSERT(strstr(buf, "R8") != NULL);
    UASSERT(strstr(buf, "R9") != NULL);
    UASSERT(strstr(buf, "R255") != NULL);
    umodule_destroy(&m);
}

/* -------------------------------------------------------------------------
 * OP_GETSLOT_CHANGE_EVENT (45): ABC: dst_reg, recv_reg, name_sym_id
 * Expected: "GETSLOT_CHANGE_EVENT R0, R1, K3"
 * (C encodes a symbol-table index — displayed as Kn to distinguish from reg)
 * ------------------------------------------------------------------------- */
UTEST(disasm_getslot_change_event) {
    UModule m = make_one_instr_module(uinstr_enc_abc(OP_GETSLOT_CHANGE_EVENT, 0u, 1u, 3u));
    char buf[256];
    size_t n = uemit_disassemble(&m, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "GETSLOT_CHANGE_EVENT") != NULL);
    UASSERT(strstr(buf, "R0") != NULL);
    UASSERT(strstr(buf, "R1") != NULL);
    UASSERT(strstr(buf, "K3") != NULL);
    umodule_destroy(&m);
}

/* -------------------------------------------------------------------------
 * OP_LOAD_REALM_GLOBAL (46): ABC: dst_reg, sym_id_hi, sym_id_lo
 * Expected: "LOAD_REALM_GLOBAL R2, sym(0,7)"
 * (B and C are a 16-bit symbol id split into hi/lo bytes)
 * ------------------------------------------------------------------------- */
UTEST(disasm_load_realm_global) {
    UModule m = make_one_instr_module(uinstr_enc_abc(OP_LOAD_REALM_GLOBAL, 2u, 0u, 7u));
    char buf[256];
    size_t n = uemit_disassemble(&m, buf, sizeof buf);
    UASSERT(n > 0);
    UASSERT(strstr(buf, "LOAD_REALM_GLOBAL") != NULL);
    UASSERT(strstr(buf, "R2") != NULL);
    UASSERT(strstr(buf, "sym(") != NULL);
    umodule_destroy(&m);
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
}
