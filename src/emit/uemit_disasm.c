/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_disasm.c — bytecode disassembler.
 * Extracted from uemit.c during v0.5.4-decompose (EMIT-045 #7).
 * EMIT-035: 30-arm switch replaced with static op_disasm[] table. */

#include "uemit_internal.h"
#include "chunk/uchunk.h"
#include <stdint.h>

#if __STDC_HOSTED__
#  include <inttypes.h>
#  include <stdarg.h>
#  include <stdio.h>   /* vsnprintf */

/* snprintf into (buf+off, cap-off), advancing *off.  Returns false when
   capacity is exhausted; always null-terminates buf when cap > 0. */
static bool dis_printf(char *buf, const size_t cap, size_t *off,
                       const char *fmt, ...) {
    va_list ap;
    int n;
    if (*off >= cap) return false;
    va_start(ap, fmt);
    /* False positive: ap is initialized by va_start, consumed by vsnprintf,
     * then cleared by va_end.  Analyzer cannot see through the va_list
     * contract on the vsnprintf prototype. */
    n = vsnprintf(buf + *off, cap - *off, fmt, ap);  /* NOLINT(clang-analyzer-valist.Uninitialized) — ap initialized by va_start above */
    va_end(ap);
    if (n < 0) return false;
    if ((size_t)n >= cap - *off) {
        *off = cap - 1U;
        buf[*off] = '\0';
        return false;
    }
    *off += (size_t)n;
    return true;
}

/* Format-function type: write one instruction line into the dis buffer.
 * ip    — pointer to the loop index; CLOSURE advances it to skip upval pseudos.
 * ins   — the raw 32-bit instruction word.
 * module — the containing module (needed by CLOSURE for upval descriptors).
 * Returns false when the buffer capacity is exhausted. */
typedef bool (*UDisFormatFn)(char *buf, size_t cap, size_t *off,
                             size_t *ip, uint32_t ins,
                             const UProto *module);

/* --- Per-opcode format helpers --- */

static bool fmt_loadk(char *buf, size_t cap, size_t *off,
                      size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  LOADK R%u, K%u\n",
                      *ip, (unsigned)uinstr_a(ins), (unsigned)uinstr_bx(ins));
}

static bool fmt_ret(char *buf, size_t cap, size_t *off,
                    size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  RET R%u\n",
                      *ip, (unsigned)uinstr_a(ins));
}

static bool fmt_neg(char *buf, size_t cap, size_t *off,
                    size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  NEG R%u, R%u\n",
                      *ip, (unsigned)uinstr_a(ins), (unsigned)uinstr_b(ins));
}

static bool fmt_closure(char *buf, size_t cap, size_t *off,
                        size_t *ip, uint32_t ins, const UProto *module) {
    const uint16_t bx = uinstr_bx(ins);
    bool ok = dis_printf(buf, cap, off, "%04zu  CLOSURE R%u, P%u\n",
                         *ip, (unsigned)uinstr_a(ins), (unsigned)bx);
    if (!ok) return false;
    if (module != NULL
        && bx < module->nested_count
        && module->nested[bx] != NULL) {
        const UProto *child = module->nested[bx];
        const UProto *rp    = module;
        uint8_t u;
        for (u = 0; u < child->nupvals &&
             (*ip + 1U + (size_t)u) < rp->instr_count; u++) {
            uint32_t pi = rp->instructions[*ip + 1U + u];
            ok = dis_printf(buf, cap, off,
                "    upval[%u]: %s parent_idx=%u\n",
                (unsigned)u,
                uinstr_b(pi) ? "in_stack" : "from_upval",
                (unsigned)uinstr_c(pi));
            if (!ok) return false;
        }
        *ip += child->nupvals;  /* skip upvalue prelude instructions */
    }
    return true;
}

static bool fmt_jmp(char *buf, size_t cap, size_t *off,
                    size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  JMP %d\n",
                      *ip, (int)uinstr_bx(ins) - (int)UEMIT_JMP_BIAS);
}

static bool fmt_loadnil(char *buf, size_t cap, size_t *off,
                        size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  LOADNIL R%u\n",
                      *ip, (unsigned)uinstr_a(ins));
}

static bool fmt_loadbool(char *buf, size_t cap, size_t *off,
                         size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  LOADBOOL R%u, %s%s\n",
                      *ip, (unsigned)uinstr_a(ins),
                      uinstr_b(ins) ? "true" : "false",
                      uinstr_c(ins) ? " (skip)" : "");
}

static bool fmt_loadvoid(char *buf, size_t cap, size_t *off,
                         size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  LOADVOID R%u\n",
                      *ip, (unsigned)uinstr_a(ins));
}

static bool fmt_getupval(char *buf, size_t cap, size_t *off,
                         size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  GETUPVAL R%u, U%u\n",
                      *ip, (unsigned)uinstr_a(ins), (unsigned)uinstr_b(ins));
}

static bool fmt_setupval(char *buf, size_t cap, size_t *off,
                         size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  SETUPVAL U%u, R%u\n",
                      *ip, (unsigned)uinstr_b(ins), (unsigned)uinstr_a(ins));
}

static bool fmt_close(char *buf, size_t cap, size_t *off,
                      size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  CLOSE R%u..\n",
                      *ip, (unsigned)uinstr_a(ins));
}

static bool fmt_call(char *buf, size_t cap, size_t *off,
                     size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    uint8_t c = uinstr_c(ins);
    bool is_method = (c & 0x80U) != 0U;
    int  nresults = (int)(c & 0x7FU) - 1;
    int  b        = (int)uinstr_b(ins);
    int  nargs    = is_method ? (b - 2) : (b - 1);
    return dis_printf(buf, cap, off,
                      "%04zu  CALL%s R%u, %d args, %d results\n",
                      *ip, is_method ? " [method]" : "",
                      (unsigned)uinstr_a(ins), nargs, nresults);
}

static bool fmt_test(char *buf, size_t cap, size_t *off,
                     size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  TEST R%u, %s\n",
                      *ip, (unsigned)uinstr_a(ins),
                      uinstr_c(ins) ? "skip-if-truthy" : "skip-if-falsy");
}

static bool fmt_testset(char *buf, size_t cap, size_t *off,
                        size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  TESTSET R%u, R%u, %u\n",
                      *ip, (unsigned)uinstr_a(ins), (unsigned)uinstr_b(ins),
                      (unsigned)uinstr_c(ins));
}

static bool fmt_eq(char *buf, size_t cap, size_t *off,
                   size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  EQ %s R%u, R%u\n",
                      *ip, uinstr_a(ins) ? "==" : "!=",
                      (unsigned)uinstr_b(ins),
                      (unsigned)uinstr_c(ins));
}

static bool fmt_neq(char *buf, size_t cap, size_t *off,
                    size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  NEQ R%u, R%u\n",
                      *ip, (unsigned)uinstr_b(ins),
                      (unsigned)uinstr_c(ins));
}

static bool fmt_lt(char *buf, size_t cap, size_t *off,
                   size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  LT R%u, R%u (%s)\n",
                      *ip, (unsigned)uinstr_b(ins),
                      (unsigned)uinstr_c(ins),
                      uinstr_a(ins) ? "<" : ">=");
}

static bool fmt_le(char *buf, size_t cap, size_t *off,
                   size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  LE R%u, R%u (%s)\n",
                      *ip, (unsigned)uinstr_b(ins),
                      (unsigned)uinstr_c(ins),
                      uinstr_a(ins) ? "<=" : ">");
}

static bool fmt_yield(char *buf, size_t cap, size_t *off,
                      size_t *ip, uint32_t ins, const UProto *module) {
    (void)ins; (void)module;
    return dis_printf(buf, cap, off, "%04zu  YIELD\n", *ip);
}

static bool fmt_fork_detach(char *buf, size_t cap, size_t *off,
                            size_t *ip, uint32_t ins, const UProto *module) {
    (void)ins; (void)module;
    return dis_printf(buf, cap, off, "%04zu  FORK_DETACH (reserved)\n", *ip);
}

static bool fmt_fork_join(char *buf, size_t cap, size_t *off,
                          size_t *ip, uint32_t ins, const UProto *module) {
    (void)ins; (void)module;
    return dis_printf(buf, cap, off, "%04zu  FORK_JOIN (reserved)\n", *ip);
}

static bool fmt_join_wait(char *buf, size_t cap, size_t *off,
                          size_t *ip, uint32_t ins, const UProto *module) {
    (void)ins; (void)module;
    return dis_printf(buf, cap, off, "%04zu  JOIN_WAIT (reserved)\n", *ip);
}

static bool fmt_getslot(char *buf, size_t cap, size_t *off,
                        size_t *ip, uint32_t ins, const UProto *module) {
    (void)ins; (void)module;
    return dis_printf(buf, cap, off, "%04zu  GETSLOT (reserved M4)\n", *ip);
}

static bool fmt_setslot(char *buf, size_t cap, size_t *off,
                        size_t *ip, uint32_t ins, const UProto *module) {
    (void)ins; (void)module;
    return dis_printf(buf, cap, off, "%04zu  SETSLOT (reserved M4)\n", *ip);
}

/* M5 reactive runtime — spec #2: at/whenever/waituntil */

static bool fmt_at_install(char *buf, size_t cap, size_t *off,
                           size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  AT_INSTALL R%u, R%u, R%u\n",
                      *ip, (unsigned)uinstr_a(ins),
                      (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
}

static bool fmt_at_sync_install(char *buf, size_t cap, size_t *off,
                                size_t *ip, uint32_t ins,
                                const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  AT_SYNC_INSTALL R%u, R%u, R%u\n",
                      *ip, (unsigned)uinstr_a(ins),
                      (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
}

static bool fmt_whenever_install(char *buf, size_t cap, size_t *off,
                                 size_t *ip, uint32_t ins,
                                 const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off,
                      "%04zu  WHENEVER_INSTALL R%u, R%u, R%u\n",
                      *ip, (unsigned)uinstr_a(ins),
                      (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
}

static bool fmt_waituntil_install(char *buf, size_t cap, size_t *off,
                                  size_t *ip, uint32_t ins,
                                  const UProto *module) {
    (void)module;
    /* cond_reg only; B and C are unused (zero). */
    return dis_printf(buf, cap, off, "%04zu  WAITUNTIL_INSTALL R%u\n",
                      *ip, (unsigned)uinstr_a(ins));
}

/* M5 reactive runtime — spec #3: event syncEmit + tag.enter/leave */

static bool fmt_at_event_install(char *buf, size_t cap, size_t *off,
                                 size_t *ip, uint32_t ins,
                                 const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off,
                      "%04zu  AT_EVENT_INSTALL R%u, R%u, R%u\n",
                      *ip, (unsigned)uinstr_a(ins),
                      (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
}

static bool fmt_at_event_sync_install(char *buf, size_t cap, size_t *off,
                                      size_t *ip, uint32_t ins,
                                      const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off,
                      "%04zu  AT_EVENT_SYNC_INSTALL R%u, R%u, R%u\n",
                      *ip, (unsigned)uinstr_a(ins),
                      (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
}

/* M5 reactive runtime — spec #4: slot-change events */

static bool fmt_getslot_change_event(char *buf, size_t cap, size_t *off,
                                     size_t *ip, uint32_t ins,
                                     const UProto *module) {
    (void)module;
    /* C is a symbol-table index (not a register); display as Kn. */
    return dis_printf(buf, cap, off,
                      "%04zu  GETSLOT_CHANGE_EVENT R%u, R%u, K%u\n",
                      *ip, (unsigned)uinstr_a(ins),
                      (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
}

/* M5 reactive runtime — spec #5: globals exposure */

static bool fmt_load_realm_global(char *buf, size_t cap, size_t *off,
                                  size_t *ip, uint32_t ins,
                                  const UProto *module) {
    (void)module;
    /* B=sym_id_hi, C=sym_id_lo (16-bit symbol id split into two bytes). */
    return dis_printf(buf, cap, off,
                      "%04zu  LOAD_REALM_GLOBAL R%u, sym(%u,%u)\n",
                      *ip, (unsigned)uinstr_a(ins),
                      (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
}

static bool fmt_load_recv(char *buf, size_t cap, size_t *off,
                          size_t *ip, uint32_t ins,
                          const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  LOAD_RECV R%u\n",
                      *ip, (unsigned)uinstr_a(ins));
}

static bool fmt_self(char *buf, size_t cap, size_t *off,
                     size_t *ip, uint32_t ins,
                     const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off,
                      "%04zu  SELF R%u, R%u, ic[%u]   ; R%u := lookup, R%u := R%u\n",
                      *ip, (unsigned)uinstr_a(ins), (unsigned)uinstr_b(ins),
                      (unsigned)uinstr_c(ins),
                      (unsigned)uinstr_a(ins),
                      (unsigned)(uinstr_a(ins) + 1U),
                      (unsigned)uinstr_b(ins));
}

/* --- opname helper (used by the generic fallback in uemit_disassemble) --- */

static const char *opname(const UOpcode op) {
    switch (op) {
    case OP_LOADK:        return "LOADK";
    case OP_MOVE:         return "MOVE";
    case OP_ADD:          return "ADD";
    case OP_SUB:          return "SUB";
    case OP_MUL:          return "MUL";
    case OP_DIV:          return "DIV";
    case OP_NEG:          return "NEG";
    case OP_RET:          return "RET";
    case OP_LOADNIL:      return "LOADNIL";
    case OP_LOADBOOL:     return "LOADBOOL";
    case OP_LOADVOID:     return "LOADVOID";
    case OP_GETUPVAL:     return "GETUPVAL";
    case OP_SETUPVAL:     return "SETUPVAL";
    case OP_CLOSURE:      return "CLOSURE";
    case OP_CLOSE:        return "CLOSE";
    case OP_CALL:         return "CALL";
    case OP_JMP:          return "JMP";
    case OP_TEST:         return "TEST";
    case OP_TESTSET:      return "TESTSET";
    case OP_EQ:           return "EQ";
    case OP_NEQ:          return "NEQ";
    case OP_LT:           return "LT";
    case OP_LE:           return "LE";
    case OP_YIELD:        return "YIELD";
    case OP_FORK_DETACH:  return "FORK_DETACH";
    case OP_FORK_JOIN:    return "FORK_JOIN";
    case OP_JOIN_WAIT:    return "JOIN_WAIT";
    case OP_GETSLOT:      return "GETSLOT";
    case OP_SETSLOT:      return "SETSLOT";
    case OP_THROW:        return "THROW";
    case OP_TAG_STOP:     return "TAG_STOP";
    case OP_TRY_BEGIN:    return "TRY_BEGIN";
    case OP_TRY_END:      return "TRY_END";
    case OP_PUSH_TAG:     return "PUSH_TAG";
    case OP_POP_TAG:      return "POP_TAG";
    case OP_PUSH_FRAME_GUARD:     return "PUSH_FRAME_GUARD";
    case OP_RESUME:               return "RESUME";
    case OP_LOAD_CATCH_VALUE:     return "LOAD_CATCH_VALUE";
    /* M5 reactive runtime stubs */
    case OP_AT_INSTALL:           return "AT_INSTALL";
    case OP_AT_SYNC_INSTALL:      return "AT_SYNC_INSTALL";
    case OP_WHENEVER_INSTALL:     return "WHENEVER_INSTALL";
    case OP_WAITUNTIL_INSTALL:    return "WAITUNTIL_INSTALL";
    case OP_AT_EVENT_INSTALL:     return "AT_EVENT_INSTALL";
    case OP_AT_EVENT_SYNC_INSTALL:return "AT_EVENT_SYNC_INSTALL";
    case OP_GETSLOT_CHANGE_EVENT: return "GETSLOT_CHANGE_EVENT";
    case OP_LOAD_REALM_GLOBAL:    return "LOAD_REALM_GLOBAL";
    case OP_LOAD_RECV:            return "LOAD_RECV";
    case OP_SELF:                 return "SELF";
    case OP_MAX:                  break;
    }
    return "OP?";
}

/* --- Dispatch table (indexed by UOpcode value 0..OP_MAX-1) ---
 *
 * NULL entries fall through to the generic R%u, R%u, R%u fallback in
 * uemit_disassemble.  Opcodes not listed in the original switch (MOVE,
 * ADD, SUB, MUL, DIV, THROW, TAG_STOP, TRY_BEGIN, TRY_END, PUSH_TAG,
 * POP_TAG, PUSH_FRAME_GUARD, RESUME, LOAD_CATCH_VALUE) keep the generic
 * three-register format from the original default arm. */
static const UDisFormatFn op_disasm[OP_MAX] = {
    /* 0  OP_LOADK              */ fmt_loadk,
    /* 1  OP_MOVE               */ NULL,
    /* 2  OP_ADD                */ NULL,
    /* 3  OP_SUB                */ NULL,
    /* 4  OP_MUL                */ NULL,
    /* 5  OP_DIV                */ NULL,
    /* 6  OP_NEG                */ fmt_neg,
    /* 7  OP_RET                */ fmt_ret,
    /* 8  OP_LOADNIL            */ fmt_loadnil,
    /* 9  OP_LOADBOOL           */ fmt_loadbool,
    /* 10 OP_LOADVOID           */ fmt_loadvoid,
    /* 11 OP_GETUPVAL           */ fmt_getupval,
    /* 12 OP_SETUPVAL           */ fmt_setupval,
    /* 13 OP_CLOSURE            */ fmt_closure,
    /* 14 OP_CLOSE              */ fmt_close,
    /* 15 OP_CALL               */ fmt_call,
    /* 16 OP_JMP                */ fmt_jmp,
    /* 17 OP_TEST               */ fmt_test,
    /* 18 OP_TESTSET            */ fmt_testset,
    /* 19 OP_EQ                 */ fmt_eq,
    /* 20 OP_NEQ                */ fmt_neq,
    /* 21 OP_LT                 */ fmt_lt,
    /* 22 OP_LE                 */ fmt_le,
    /* 23 OP_YIELD              */ fmt_yield,
    /* 24 OP_FORK_DETACH        */ fmt_fork_detach,
    /* 25 OP_FORK_JOIN          */ fmt_fork_join,
    /* 26 OP_JOIN_WAIT          */ fmt_join_wait,
    /* 27 OP_GETSLOT            */ fmt_getslot,
    /* 28 OP_SETSLOT            */ fmt_setslot,
    /* 29 OP_THROW              */ NULL,
    /* 30 OP_TAG_STOP           */ NULL,
    /* 31 OP_TRY_BEGIN          */ NULL,
    /* 32 OP_TRY_END            */ NULL,
    /* 33 OP_PUSH_TAG           */ NULL,
    /* 34 OP_POP_TAG            */ NULL,
    /* 35 OP_PUSH_FRAME_GUARD   */ NULL,
    /* 36 OP_RESUME             */ NULL,
    /* 37 OP_LOAD_CATCH_VALUE   */ NULL,
    /* 38 OP_AT_INSTALL         */ fmt_at_install,
    /* 39 OP_AT_SYNC_INSTALL    */ fmt_at_sync_install,
    /* 40 OP_WHENEVER_INSTALL   */ fmt_whenever_install,
    /* 41 OP_WAITUNTIL_INSTALL  */ fmt_waituntil_install,
    /* 42 OP_AT_EVENT_INSTALL   */ fmt_at_event_install,
    /* 43 OP_AT_EVENT_SYNC_INSTALL */ fmt_at_event_sync_install,
    /* 44 OP_GETSLOT_CHANGE_EVENT  */ fmt_getslot_change_event,
    /* 45 OP_LOAD_REALM_GLOBAL  */ fmt_load_realm_global,
    /* 46 OP_LOAD_RECV          */ fmt_load_recv,
    /* 47 OP_SELF               */ fmt_self,
};

size_t uemit_disassemble(const UProto *module, char *buf, const size_t cap) {
    size_t off;
    size_t i;
    if (cap == 0 || buf == NULL) return 0;
    buf[0] = '\0';
    off = 0;
    /* v0.9.2: module IS the root UProto. */
    const UProto *rp = module;
    if (rp == NULL || rp->instr_count == 0) {
        dis_printf(buf, cap, &off, "(empty)\n");
        return off;
    }
    for (i = 0; i < rp->instr_count; i++) {
        const uint32_t ins = rp->instructions[i];
        const UOpcode  op  = uinstr_op(ins);
        bool ok;
        if ((unsigned)op < (unsigned)OP_MAX && op_disasm[op] != NULL) {
            ok = op_disasm[op](buf, cap, &off, &i, ins, module);
        } else {
            ok = dis_printf(buf, cap, &off, "%04zu  %s R%u, R%u, R%u\n",
                            i, opname(op), (unsigned)uinstr_a(ins),
                            (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
        }
        if (!ok) return off;
    }
    if (!dis_printf(buf, cap, &off, "; constants:\n")) return off;
    for (i = 0; i < rp->const_count; i++) {
        bool ok;
        if (rp->constants[i].kind == (uint8_t)UVAL_INT) {
            ok = dis_printf(buf, cap, &off, ";   K%zu = INT %" PRId64 "\n",
                            i, rp->constants[i].v.i);
        } else {
            ok = dis_printf(buf, cap, &off, ";   K%zu = ?\n", i);
        }
        if (!ok) return off;
    }
    return off;
}

#else  /* freestanding */

size_t uemit_disassemble(const UProto *module, char *buf, const size_t cap) {
    (void)module;
    if (cap > 0 && buf != NULL) buf[0] = '\0';
    return 0;
}

#endif  /* __STDC_HOSTED__ */
