/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_disasm.c — bytecode disassembler.
 * Extracted from uemit.c during v0.5.4-decompose (EMIT-045 #7). */

#include "uemit_internal.h"

#if __STDC_HOSTED__
#  include <inttypes.h>
#  include <stdarg.h>
#  include <stdio.h>   /* vsnprintf */

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
    case OP_INVOKE:               return "INVOKE";
    /* M5 reactive runtime stubs */
    case OP_AT_INSTALL:           return "AT_INSTALL";
    case OP_AT_SYNC_INSTALL:      return "AT_SYNC_INSTALL";
    case OP_WHENEVER_INSTALL:     return "WHENEVER_INSTALL";
    case OP_WAITUNTIL_INSTALL:    return "WAITUNTIL_INSTALL";
    case OP_AT_EVENT_INSTALL:     return "AT_EVENT_INSTALL";
    case OP_AT_EVENT_SYNC_INSTALL:return "AT_EVENT_SYNC_INSTALL";
    case OP_GETSLOT_CHANGE_EVENT: return "GETSLOT_CHANGE_EVENT";
    case OP_LOAD_REALM_GLOBAL:    return "LOAD_REALM_GLOBAL";
    case OP_MAX:                  break;
    }
    return "OP?";
}

/* snprintf into (buf+off, cap-off), advancing *off.  Returns false when
   capacity is exhausted; always null-terminates buf when cap > 0. */
static bool dis_printf(char *buf, const size_t cap, size_t *off,
                       const char *fmt, ...) {
    va_list ap;
    int n;
    if (*off >= cap) return false;
    va_start(ap, fmt);
    n = vsnprintf(buf + *off, cap - *off, fmt, ap);
    va_end(ap);
    if (n < 0) return false;
    if ((size_t)n >= cap - *off) {
        *off = cap - 1u;
        buf[*off] = '\0';
        return false;
    }
    *off += (size_t)n;
    return true;
}

size_t uemit_disassemble(const UModule *module, char *buf, const size_t cap) {
    size_t off;
    size_t i;
    if (cap == 0 || buf == NULL) return 0;
    buf[0] = '\0';
    off = 0;
    if (module->instr_count == 0) {
        dis_printf(buf, cap, &off, "(empty)\n");
        return off;
    }
    for (i = 0; i < module->instr_count; i++) {
        const uint32_t ins = module->instructions[i];
        const UOpcode  op  = uinstr_op(ins);
        const uint8_t  a   = uinstr_a(ins);
        bool ok;
        switch (op) {
        case OP_LOADK:
            ok = dis_printf(buf, cap, &off, "%04zu  LOADK R%u, K%u\n",
                            i, (unsigned)a, (unsigned)uinstr_bx(ins));
            break;
        case OP_RET:
            ok = dis_printf(buf, cap, &off, "%04zu  RET R%u\n",
                            i, (unsigned)a);
            break;
        case OP_NEG:
            ok = dis_printf(buf, cap, &off, "%04zu  NEG R%u, R%u\n",
                            i, (unsigned)a, (unsigned)uinstr_b(ins));
            break;
        case OP_CLOSURE: {
            const uint16_t bx = uinstr_bx(ins);
            ok = dis_printf(buf, cap, &off, "%04zu  CLOSURE R%u, P%u\n",
                            i, (unsigned)a, (unsigned)bx);
            if (!ok) return off;
            if (bx < module->nested_count && module->nested[bx] != NULL) {
                const UProto *child = module->nested[bx];
                uint8_t u;
                for (u = 0; u < child->nupvals &&
                     (i + 1u + (size_t)u) < module->instr_count; u++) {
                    uint32_t pi = module->instructions[i + 1u + u];
                    ok = dis_printf(buf, cap, &off,
                        "    upval[%u]: %s parent_idx=%u\n",
                        (unsigned)u,
                        uinstr_b(pi) ? "in_stack" : "from_upval",
                        (unsigned)uinstr_c(pi));
                    if (!ok) return off;
                }
                i += child->nupvals;  /* skip upvalue prelude instructions */
            }
            break;
        }
        case OP_JMP:
            ok = dis_printf(buf, cap, &off, "%04zu  JMP %d\n",
                            i, (int)uinstr_bx(ins) - 32768);
            break;
        case OP_LOADNIL:
            ok = dis_printf(buf, cap, &off, "%04zu  LOADNIL R%u\n",
                            i, (unsigned)a);
            break;
        case OP_LOADBOOL:
            ok = dis_printf(buf, cap, &off, "%04zu  LOADBOOL R%u, %s%s\n",
                            i, (unsigned)a,
                            uinstr_b(ins) ? "true" : "false",
                            uinstr_c(ins) ? " (skip)" : "");
            break;
        case OP_LOADVOID:
            ok = dis_printf(buf, cap, &off, "%04zu  LOADVOID R%u\n",
                            i, (unsigned)a);
            break;
        case OP_GETUPVAL:
            ok = dis_printf(buf, cap, &off, "%04zu  GETUPVAL R%u, U%u\n",
                            i, (unsigned)a, (unsigned)uinstr_b(ins));
            break;
        case OP_SETUPVAL:
            ok = dis_printf(buf, cap, &off, "%04zu  SETUPVAL U%u, R%u\n",
                            i, (unsigned)uinstr_b(ins), (unsigned)a);
            break;
        case OP_CLOSE:
            ok = dis_printf(buf, cap, &off, "%04zu  CLOSE R%u..\n",
                            i, (unsigned)a);
            break;
        case OP_CALL:
            ok = dis_printf(buf, cap, &off, "%04zu  CALL R%u, %d args, %d results\n",
                            i, (unsigned)a,
                            (int)uinstr_b(ins) - 1,
                            (int)uinstr_c(ins) - 1);
            break;
        case OP_TEST:
            ok = dis_printf(buf, cap, &off, "%04zu  TEST R%u, %s\n",
                            i, (unsigned)a,
                            uinstr_c(ins) ? "skip-if-truthy" : "skip-if-falsy");
            break;
        case OP_TESTSET:
            ok = dis_printf(buf, cap, &off, "%04zu  TESTSET R%u, R%u, %u\n",
                            i, (unsigned)a, (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins));
            break;
        case OP_EQ:
            ok = dis_printf(buf, cap, &off, "%04zu  EQ %s R%u, R%u\n",
                            i, a ? "==" : "!=",
                            (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins));
            break;
        case OP_NEQ:
            ok = dis_printf(buf, cap, &off, "%04zu  NEQ R%u, R%u\n",
                            i, (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins));
            break;
        case OP_LT:
            ok = dis_printf(buf, cap, &off, "%04zu  LT R%u, R%u (%s)\n",
                            i, (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins),
                            a ? "<" : ">=");
            break;
        case OP_LE:
            ok = dis_printf(buf, cap, &off, "%04zu  LE R%u, R%u (%s)\n",
                            i, (unsigned)uinstr_b(ins),
                            (unsigned)uinstr_c(ins),
                            a ? "<=" : ">");
            break;
        case OP_YIELD:
            ok = dis_printf(buf, cap, &off, "%04zu  YIELD\n", i);
            break;
        case OP_FORK_DETACH:
            ok = dis_printf(buf, cap, &off, "%04zu  FORK_DETACH (reserved)\n", i);
            break;
        case OP_FORK_JOIN:
            ok = dis_printf(buf, cap, &off, "%04zu  FORK_JOIN (reserved)\n", i);
            break;
        case OP_JOIN_WAIT:
            ok = dis_printf(buf, cap, &off, "%04zu  JOIN_WAIT (reserved)\n", i);
            break;
        case OP_GETSLOT:
            ok = dis_printf(buf, cap, &off, "%04zu  GETSLOT (reserved M4)\n", i);
            break;
        case OP_SETSLOT:
            ok = dis_printf(buf, cap, &off, "%04zu  SETSLOT (reserved M4)\n", i);
            break;
        /* M5 reactive runtime — spec #2: at/whenever/waituntil */
        case OP_AT_INSTALL:
            ok = dis_printf(buf, cap, &off, "%04zu  AT_INSTALL R%u, R%u, R%u\n",
                            i, (unsigned)a,
                            (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
            break;
        case OP_AT_SYNC_INSTALL:
            ok = dis_printf(buf, cap, &off, "%04zu  AT_SYNC_INSTALL R%u, R%u, R%u\n",
                            i, (unsigned)a,
                            (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
            break;
        case OP_WHENEVER_INSTALL:
            ok = dis_printf(buf, cap, &off, "%04zu  WHENEVER_INSTALL R%u, R%u, R%u\n",
                            i, (unsigned)a,
                            (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
            break;
        case OP_WAITUNTIL_INSTALL:
            /* cond_reg only; B and C are unused (zero). */
            ok = dis_printf(buf, cap, &off, "%04zu  WAITUNTIL_INSTALL R%u\n",
                            i, (unsigned)a);
            break;
        /* M5 reactive runtime — spec #3: event syncEmit + tag.enter/leave */
        case OP_AT_EVENT_INSTALL:
            ok = dis_printf(buf, cap, &off, "%04zu  AT_EVENT_INSTALL R%u, R%u, R%u\n",
                            i, (unsigned)a,
                            (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
            break;
        case OP_AT_EVENT_SYNC_INSTALL:
            ok = dis_printf(buf, cap, &off, "%04zu  AT_EVENT_SYNC_INSTALL R%u, R%u, R%u\n",
                            i, (unsigned)a,
                            (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
            break;
        /* M5 reactive runtime — spec #4: slot-change events */
        case OP_GETSLOT_CHANGE_EVENT:
            /* C is a symbol-table index (not a register); display as Kn. */
            ok = dis_printf(buf, cap, &off, "%04zu  GETSLOT_CHANGE_EVENT R%u, R%u, K%u\n",
                            i, (unsigned)a,
                            (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
            break;
        /* M5 reactive runtime — spec #5: globals exposure */
        case OP_LOAD_REALM_GLOBAL:
            /* B=sym_id_hi, C=sym_id_lo (16-bit symbol id split into two bytes). */
            ok = dis_printf(buf, cap, &off, "%04zu  LOAD_REALM_GLOBAL R%u, sym(%u,%u)\n",
                            i, (unsigned)a,
                            (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
            break;
        default:
            ok = dis_printf(buf, cap, &off, "%04zu  %s R%u, R%u, R%u\n",
                            i, opname(op), (unsigned)a,
                            (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
            break;
        }
        if (!ok) return off;
    }
    if (!dis_printf(buf, cap, &off, "; constants:\n")) return off;
    for (i = 0; i < module->const_count; i++) {
        bool ok;
        if (module->constants[i].kind == (uint8_t)UVAL_INT) {
            ok = dis_printf(buf, cap, &off, ";   K%zu = INT %" PRId64 "\n",
                            i, module->constants[i].v.i);
        } else {
            ok = dis_printf(buf, cap, &off, ";   K%zu = ?\n", i);
        }
        if (!ok) return off;
    }
    return off;
}

#else  /* freestanding */

size_t uemit_disassemble(const UModule *module, char *buf, const size_t cap) {
    (void)module;
    if (cap > 0 && buf != NULL) buf[0] = '\0';
    return 0;
}

#endif  /* __STDC_HOSTED__ */
