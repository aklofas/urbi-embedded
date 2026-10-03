/* SPDX-License-Identifier: BSD-3-Clause */

#include "uemit_internal.h"
#include "chunk/uchunk.h"
#include <stdint.h>
#include <string.h>

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

static bool fmt_fork(char *buf, size_t cap, size_t *off,
                     size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    if (uinstr_c(ins) == UFORK_JOIN)
        return dis_printf(buf, cap, off, "%04zu  FORK R%u -> R%u join\n",
                          *ip, (unsigned)uinstr_a(ins), (unsigned)uinstr_b(ins));
    return dis_printf(buf, cap, off, "%04zu  FORK R%u detach\n",
                      *ip, (unsigned)uinstr_a(ins));
}

static bool fmt_join_wait(char *buf, size_t cap, size_t *off,
                          size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  JOIN_WAIT R%u\n",
                      *ip, (unsigned)uinstr_a(ins));
}

/* The five slot opcodes share one format and print the resolved site
 * index, folding in the high bits of an OP_EXTARG right before them. */
static bool fmt_site(char *buf, size_t cap, size_t *off,
                     size_t *ip, uint32_t ins, const UProto *module) {
    uint32_t site = uinstr_c(ins);
    if (module != NULL && *ip > 0U
        && uinstr_op(module->instructions[*ip - 1U]) == OP_EXTARG)
        site |= (uint32_t)uinstr_bx(module->instructions[*ip - 1U]) << 8;
    const UOpcode op = uinstr_op(ins);
    const char *name = op == OP_GETSLOT        ? "GETSLOT"
                     : op == OP_SETSLOT        ? "SETSLOT"
                     : op == OP_SETSLOT_UPDATE ? "SETSLOT_UPDATE"
                     : op == OP_SELF           ? "SELF"
                     :                           "GETSLOT_CHANGE_EVENT";
    return dis_printf(buf, cap, off, "%04zu  %s R%u, R%u, site %u\n",
                      *ip, name, (unsigned)uinstr_a(ins),
                      (unsigned)uinstr_b(ins), (unsigned)site);
}

static bool fmt_extarg(char *buf, size_t cap, size_t *off,
                       size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  EXTARG hi=%u\n",
                      *ip, (unsigned)uinstr_bx(ins));
}

static bool fmt_scope_try(char *buf, size_t cap, size_t *off,
                          size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  SCOPE_TRY flags=%u -> %u\n",
                      *ip, (unsigned)uinstr_a(ins), (unsigned)uinstr_bx(ins));
}

static bool fmt_scope_tag(char *buf, size_t cap, size_t *off,
                          size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    if (uinstr_a(ins) == USCOPE_NO_REG)
        return dis_printf(buf, cap, off, "%04zu  SCOPE_TAG fresh -> %u\n",
                          *ip, (unsigned)uinstr_bx(ins));
    return dis_printf(buf, cap, off, "%04zu  SCOPE_TAG R%u -> %u\n",
                      *ip, (unsigned)uinstr_a(ins), (unsigned)uinstr_bx(ins));
}

static bool fmt_scope_pop(char *buf, size_t cap, size_t *off,
                          size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    const uint8_t a = uinstr_a(ins);
    return dis_printf(buf, cap, off, "%04zu  SCOPE_POP %s%s\n", *ip,
                      (a & 0x3U) == USCOPE_POP_TAG ? "tag" : "try",
                      (a & USCOPE_POP_RUN_FINALLY) != 0U ? "+finally" : "");
}

static bool fmt_unwind_to(char *buf, size_t cap, size_t *off,
                          size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  UNWIND_TO depth=%u -> %u\n",
                      *ip, (unsigned)uinstr_a(ins), (unsigned)uinstr_bx(ins));
}

static bool fmt_install(char *buf, size_t cap, size_t *off,
                        size_t *ip, uint32_t ins, const UProto *module) {
    (void)module;
    return dis_printf(buf, cap, off, "%04zu  INSTALL R%u mode=%u flags=%u\n",
                      *ip, (unsigned)uinstr_a(ins),
                      (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
}

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

/* --- opname helper (used by the generic fallback in uemit_disassemble) ---
 * Generated from uopcodes.def; covers every opcode. */

static const char * const opname_table[OP_MAX] = {
#define URBI_OP(n, u, s) #n,
#include "chunk/uopcodes.def"
#undef URBI_OP
};

static const char *opname(const UOpcode op) {
    if ((unsigned)op >= (unsigned)OP_MAX) return "OP?";
    return opname_table[(unsigned)op];
}

/* --- Dispatch table (indexed by UOpcode value 0..OP_MAX-1) ---
 *
 * NULL entries fall through to the generic "NAME R%u, R%u, R%u" format
 * in dis_proto. */
static const UDisFormatFn op_disasm[OP_MAX] = {
    [OP_LOADK]                = fmt_loadk,
    [OP_NEG]                  = fmt_neg,
    [OP_RET]                  = fmt_ret,
    [OP_LOADNIL]              = fmt_loadnil,
    [OP_LOADBOOL]             = fmt_loadbool,
    [OP_LOADVOID]             = fmt_loadvoid,
    [OP_GETUPVAL]             = fmt_getupval,
    [OP_SETUPVAL]             = fmt_setupval,
    [OP_CLOSURE]              = fmt_closure,
    [OP_CLOSE]                = fmt_close,
    [OP_CALL]                 = fmt_call,
    [OP_JMP]                  = fmt_jmp,
    [OP_TEST]                 = fmt_test,
    [OP_TESTSET]              = fmt_testset,
    [OP_EQ]                   = fmt_eq,
    [OP_LT]                   = fmt_lt,
    [OP_LE]                   = fmt_le,
    [OP_YIELD]                = fmt_yield,
    [OP_FORK]                 = fmt_fork,
    [OP_JOIN_WAIT]            = fmt_join_wait,
    [OP_GETSLOT]              = fmt_site,
    [OP_SETSLOT]              = fmt_site,
    [OP_SETSLOT_UPDATE]       = fmt_site,
    [OP_SELF]                 = fmt_site,
    [OP_GETSLOT_CHANGE_EVENT] = fmt_site,
    [OP_EXTARG]               = fmt_extarg,
    [OP_SCOPE_TRY]            = fmt_scope_try,
    [OP_SCOPE_TAG]            = fmt_scope_tag,
    [OP_SCOPE_POP]            = fmt_scope_pop,
    [OP_UNWIND_TO]            = fmt_unwind_to,
    [OP_INSTALL]              = fmt_install,
    [OP_LOAD_REALM_GLOBAL]    = fmt_load_realm_global,
    [OP_LOAD_RECV]            = fmt_load_recv,
    /* MOVE, ADD, SUB, MUL, DIV, THROW, RESUME, LOAD_CATCH_VALUE: generic */
};

/* One proto's instructions, then its constant pool. */
static bool dis_proto(char *buf, size_t cap, size_t *off, const UProto *p) {
    size_t i;
    if (p->instr_count == 0) return dis_printf(buf, cap, off, "(empty)\n");
    for (i = 0; i < p->instr_count; i++) {
        const uint32_t ins = p->instructions[i];
        const UOpcode  op  = uinstr_op(ins);
        bool ok;
        if ((unsigned)op < (unsigned)OP_MAX && op_disasm[op] != NULL) {
            ok = op_disasm[op](buf, cap, off, &i, ins, p);
        } else {
            ok = dis_printf(buf, cap, off, "%04zu  %s R%u, R%u, R%u\n",
                            i, opname(op), (unsigned)uinstr_a(ins),
                            (unsigned)uinstr_b(ins), (unsigned)uinstr_c(ins));
        }
        if (!ok) return false;
    }
    if (!dis_printf(buf, cap, off, "; constants:\n")) return false;
    for (i = 0; i < p->const_count; i++) {
        bool ok;
        if (p->constants[i].kind == (uint8_t)UVAL_INT) {
            ok = dis_printf(buf, cap, off, ";   K%zu = INT %" PRId64 "\n",
                            i, p->constants[i].v.i);
        } else {
            ok = dis_printf(buf, cap, off, ";   K%zu = ?\n", i);
        }
        if (!ok) return false;
    }
    return true;
}

/* Every nested proto below `p`, depth-first.  Each header names the
 * proto by its index in its parent's nested list -- the index a CLOSURE
 * in the parent prints -- after the parent's own path: P0, P1 below the
 * root, P0.0, P0.1 below P0.  `path` holds the parent's path ("" for the
 * root) and is extended in place for the children. */
static bool dis_nested(char *buf, size_t cap, size_t *off,
                       const UProto *p, char *path, size_t plen, size_t pcap) {
    for (size_t k = 0; k < p->nested_count; k++) {
        const UProto *child = p->nested[k];
        if (child == NULL) continue;
        int n = snprintf(path + plen, pcap - plen, "%s%zu", plen > 0 ? "." : "", k);
        size_t clen = (n < 0 || (size_t)n >= pcap - plen) ? pcap - 1U : plen + (size_t)n;
        if (!dis_printf(buf, cap, off, "; proto P%s\n", path)) return false;
        if (!dis_proto(buf, cap, off, child)) return false;
        if (!dis_nested(buf, cap, off, child, path, clen, pcap)) return false;
        path[plen] = '\0';
    }
    return true;
}

size_t uemit_disassemble(const UProto *root, char *buf, const size_t cap) {
    size_t off = 0;
    char path[128];
    if (cap == 0 || buf == NULL) return 0;
    buf[0] = '\0';
    path[0] = '\0';
    if (root == NULL || root->instr_count == 0) {
        dis_printf(buf, cap, &off, "(empty)\n");
        return off;
    }
    if (dis_proto(buf, cap, &off, root)) (void)dis_nested(buf, cap, &off, root, path, 0U, sizeof path);
    return off;
}

/* completeness check: returns 1 if opname() returns a non-fallback string
 * for every opcode in [0, OP_MAX).  Called from the disasm completeness
 * unit test. */
int urbi_emit_disasm_opnames_complete(void) {
    for (int op = 0; op < (int)OP_MAX; op++) {
        if (strcmp(opname((UOpcode)op), "OP?") == 0)
            return 0;
    }
    return 1;
}

#else  /* freestanding */

size_t uemit_disassemble(const UProto *root, char *buf, const size_t cap) {
    (void)root;
    if (cap > 0 && buf != NULL) buf[0] = '\0';
    return 0;
}

int urbi_emit_disasm_opnames_complete(void) { return 1; }

#endif  /* __STDC_HOSTED__ */
