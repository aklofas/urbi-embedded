/* SPDX-License-Identifier: BSD-3-Clause */
/* uvm_diag.c — VM diagnostic formatting.
 * Extracted from uvm.c during v0.5.4-decompose (VM #2). */

#include "vm/uvm.h"
#include "vm/uvm_internal.h"   /* UDiagWriter typedef + forward decls */
#include "module/umodule.h"    /* UModule, UOpcode */
#include "value/uvalue.h"      /* UValKind, UVAL_* */
#include <stddef.h>
#include <stdint.h>

/* Map UValKind to a human-readable name for diagnostic messages. */
const char *kind_name(uint8_t kind) {
    switch (kind) {
        case UVAL_NIL:   return "Nil";
        case UVAL_INT:   return "Integer";
        case UVAL_FLOAT: return "Float";
        case UVAL_BOOL:  return "Bool";
        case UVAL_STR:   return "String";
    }
    return "unknown";
}

/* Map UOpcode to its mnemonic name for diagnostic messages. */
const char *op_name(uint8_t op) {
    switch (op) {
        case OP_LOADK:          return "OP_LOADK";
        case OP_MOVE:           return "OP_MOVE";
        case OP_ADD:            return "OP_ADD";
        case OP_SUB:            return "OP_SUB";
        case OP_MUL:            return "OP_MUL";
        case OP_DIV:            return "OP_DIV";
        case OP_NEG:            return "OP_NEG";
        case OP_RET:            return "OP_RET";
        case OP_LOADNIL:        return "OP_LOADNIL";
        case OP_LOADBOOL:       return "OP_LOADBOOL";
        case OP_LOADVOID:       return "OP_LOADVOID";
        case OP_GETUPVAL:       return "OP_GETUPVAL";
        case OP_SETUPVAL:       return "OP_SETUPVAL";
        case OP_CLOSURE:        return "OP_CLOSURE";
        case OP_CLOSE:          return "OP_CLOSE";
        case OP_CALL:           return "OP_CALL";
        case OP_JMP:            return "OP_JMP";
        case OP_TEST:           return "OP_TEST";
        case OP_TESTSET:        return "OP_TESTSET";
        case OP_EQ:             return "OP_EQ";
        case OP_NEQ:            return "OP_NEQ";
        case OP_LT:             return "OP_LT";
        case OP_LE:             return "OP_LE";
        case OP_YIELD:          return "OP_YIELD";
        case OP_FORK_DETACH:    return "OP_FORK_DETACH";
        case OP_FORK_JOIN:      return "OP_FORK_JOIN";
        case OP_JOIN_WAIT:      return "OP_JOIN_WAIT";
        case OP_GETSLOT:        return "OP_GETSLOT";
        case OP_SETSLOT:        return "OP_SETSLOT";
        /* M3 row 7 control-transfer opcodes */
        case OP_THROW:          return "OP_THROW";
        case OP_TAG_STOP:       return "OP_TAG_STOP";
        case OP_TRY_BEGIN:      return "OP_TRY_BEGIN";
        case OP_TRY_END:        return "OP_TRY_END";
        case OP_PUSH_TAG:       return "OP_PUSH_TAG";
        case OP_POP_TAG:        return "OP_POP_TAG";
        case OP_PUSH_FRAME_GUARD:     return "OP_PUSH_FRAME_GUARD";
        case OP_RESUME:               return "OP_RESUME";
        case OP_LOAD_CATCH_VALUE:     return "OP_LOAD_CATCH_VALUE";
        /* M5 reactive runtime stubs */
        case OP_AT_INSTALL:           return "OP_AT_INSTALL";
        case OP_AT_SYNC_INSTALL:      return "OP_AT_SYNC_INSTALL";
        case OP_WHENEVER_INSTALL:     return "OP_WHENEVER_INSTALL";
        case OP_WAITUNTIL_INSTALL:    return "OP_WAITUNTIL_INSTALL";
        case OP_AT_EVENT_INSTALL:     return "OP_AT_EVENT_INSTALL";
        case OP_AT_EVENT_SYNC_INSTALL:return "OP_AT_EVENT_SYNC_INSTALL";
        case OP_GETSLOT_CHANGE_EVENT: return "OP_GETSLOT_CHANGE_EVENT";
        case OP_LOAD_REALM_GLOBAL:    return "OP_LOAD_REALM_GLOBAL";
        case OP_LOAD_RECV:            return "OP_LOAD_RECV";
    }
    return "unknown";
}

void diag_init(UDiagWriter *w, char *buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->used = 0;
    w->truncated = false;
    if (cap > 0) buf[0] = '\0';
}

void diag_write_cstr(UDiagWriter *w, const char *s) {
    if (w->truncated) return;
    while (*s) {
        /* Leave 4 bytes for "..." + NUL. */
        if (w->used + 4 >= w->cap) {
            w->truncated = true;
            /* Rewind to make room for ellipsis. */
            size_t ellipsis_pos = (w->cap >= 4) ? w->cap - 4 : 0;
            if (w->used > ellipsis_pos) w->used = ellipsis_pos;
            w->buf[w->used++] = '.';
            w->buf[w->used++] = '.';
            w->buf[w->used++] = '.';
            w->buf[w->used]   = '\0';
            return;
        }
        w->buf[w->used++] = *s++;
    }
    w->buf[w->used] = '\0';
}

/* Write an unsigned integer in decimal. */
void diag_write_u32(UDiagWriter *w, uint32_t n) {
    char tmp[12];
    size_t i = 0;
    if (n == 0) {
        tmp[i++] = '0';
    } else {
        while (n > 0 && i < sizeof(tmp)) {
            /* TIDY-007: route int→char through unsigned to make the
             * narrowing well-defined and silence
             * bugprone-narrowing-conversions.  '0' + (n % 10) computes in
             * int; (unsigned char) gives a defined low-byte truncation, then
             * (char) is a value-preserving same-width store. */
            tmp[i++] = (char)(unsigned char)('0' + (n % 10U));
            n /= 10;
        }
    }
    /* Reverse into the writer. */
    while (i > 0) {
        char one[2]; one[0] = tmp[--i]; one[1] = '\0';
        diag_write_cstr(w, one);
    }
}

void diag_write_size(UDiagWriter *w, size_t n) {
    /* size_t is at most 64 bits on our targets; fits in u32 for any
       realistic frame size or pc. Cap for safety. */
    if (n > UINT32_MAX) n = UINT32_MAX;
    diag_write_u32(w, (uint32_t)n);
}

void diag_write_kind_name(UDiagWriter *w, uint8_t kind) {
    diag_write_cstr(w, kind_name(kind));
}

/* Decode source line number for the given PC. Walks line_deltas from
   index 0, summing deltas; abs_lines entries (triggered by INT8_MIN
   sentinel) replace the accumulator. Returns 0 on absent syncline
   data or out-of-range pc. */
uint32_t vm_line_for_pc(const UModule *module, size_t pc) {
    /* Task 11: all chunk-top data lives on root_proto; no module fallback. */
    const UProto *rp = module->root_proto;
    if (rp == NULL) return 0;
    const int8_t       *line_deltas  = rp->line_deltas;
    const UAbsLine     *abs_lines    = rp->abs_lines;
    size_t              abs_line_cnt = rp->abs_line_count;
    size_t              instr_cnt    = rp->instr_count;
    if (line_deltas == NULL) return 0;
    if (pc >= instr_cnt) return 0;
    uint32_t line = 0;
    size_t abs_idx = 0;
    for (size_t i = 0; i <= pc; i++) {
        int8_t d = line_deltas[i];
        if (d == INT8_MIN) {
            /* Consult abs_lines; find the entry whose pc matches i. */
            while (abs_idx < abs_line_cnt &&
                   abs_lines[abs_idx].pc < i) {
                abs_idx++;
            }
            if (abs_idx < abs_line_cnt &&
                abs_lines[abs_idx].pc == i) {
                line = abs_lines[abs_idx].line;
                abs_idx++;
            }
        } else {
            /* Signed add. Cast to int32_t for the intermediate to avoid
               sign-extending an int8_t into a larger unsigned value. */
            line = (uint32_t)((int32_t)line + (int32_t)d);
        }
    }
    return line;
}

/* Format the prefix "source:line:" / "line N:" / "instr N:" into w. */
void diag_write_prefix(UDiagWriter *w, const UModule *module, size_t pc) {
    uint32_t line = vm_line_for_pc(module, pc);
    if (line == 0) {
        diag_write_cstr(w, "instr ");
        diag_write_size(w, pc);
        diag_write_cstr(w, ": ");
        return;
    }
    if (module->source_name != NULL) {
        diag_write_cstr(w, module->source_name);
        diag_write_cstr(w, ":");
    } else {
        diag_write_cstr(w, "line ");
    }
    diag_write_u32(w, line);
    diag_write_cstr(w, ": ");
}

/* Binary-op TypeError: two operand kinds reported.
   Format: "<prefix>TypeError: <OP_NAME> operands must be Integer or Float (got <Kind>, <Kind>)" */
void vm_format_type_error_binary(UVM *vm, const UModule *module, size_t pc,
                                 uint8_t op, uint8_t b_kind, uint8_t c_kind) {
    UDiagWriter w;
    diag_init(&w, vm->last_errmsg, UVM_ERRMSG_CAP);
    diag_write_prefix(&w, module, pc);
    diag_write_cstr(&w, "TypeError: ");
    diag_write_cstr(&w, op_name(op));
    diag_write_cstr(&w, " operands must be Integer or Float (got ");
    diag_write_kind_name(&w, b_kind);
    diag_write_cstr(&w, ", ");
    diag_write_kind_name(&w, c_kind);
    diag_write_cstr(&w, ")");
}

/* Unary-op TypeError: one operand kind reported. */
void vm_format_type_error_unary(UVM *vm, const UModule *module, size_t pc,
                                uint8_t op, uint8_t b_kind) {
    UDiagWriter w;
    diag_init(&w, vm->last_errmsg, UVM_ERRMSG_CAP);
    diag_write_prefix(&w, module, pc);
    diag_write_cstr(&w, "TypeError: ");
    diag_write_cstr(&w, op_name(op));
    diag_write_cstr(&w, " operand must be Integer or Float (got ");
    diag_write_kind_name(&w, b_kind);
    diag_write_cstr(&w, ")");
}

/* Format: "out of memory allocating register frame (<N> bytes requested)" */
void vm_format_oom(UVM *vm, size_t nbytes) {
    UDiagWriter w;
    diag_init(&w, vm->last_errmsg, UVM_ERRMSG_CAP);
    diag_write_cstr(&w, "out of memory allocating register frame (");
    diag_write_size(&w, nbytes);
    diag_write_cstr(&w, " bytes requested)");
}

/* Generic unsupported-opcode error message.  Used by placeholder arms
 * that will be replaced by real implementations in later tasks. */
void vm_format_type_error_msg(UVM *vm, const char *msg) {
    UDiagWriter w;
    diag_init(&w, vm->last_errmsg, UVM_ERRMSG_CAP);
    diag_write_cstr(&w, "TypeError: ");
    diag_write_cstr(&w, msg);
}
