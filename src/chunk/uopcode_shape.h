/* SPDX-License-Identifier: BSD-3-Clause */
/* uopcode_shape.h — file-private opcode-shape table for the bytecode
 * verifier.  Replaces the M1-shaped hardcoded operand checks
 * (MOD-009 + MOD-010) in src/chunk/uchunk_io.c::decode_verify.
 *
 * Add a new entry per opcode rather than extending an inline switch.
 *
 * UOpcodeFormat — top-level encoding shape:
 *   UOPF_ABC : one byte per operand A/B/C (each 0..255)
 *   UOPF_ABX : A is a byte, Bx is a uint16 (signed for OP_JMP via 32768 bias)
 *
 * UOperandKind — how the verifier interprets each byte field:
 *   UOPK_UNUSED        : field is ignored; arbitrary byte values accepted
 *   UOPK_REG           : R[k] register reference; must be <= max_reg
 *   UOPK_IMM_BOOL      : immediate 0/1; reject if > 1
 *   UOPK_IMM_FLAGS     : flags nibble high (A[7:4]); flags-only opcodes use this
 *   UOPK_IMM_REG_NIBBLE: register index in the low nibble (A[3:0]);
 *                        must be <= max_reg AND <= 15
 *   UOPK_UPVAL_IDX     : upvalue index; runtime-checked, no static range
 *                        (UClosure carries the upvalue array length)
 *   UOPK_FRAME_REG_BASE: OP_PUSH_FRAME_GUARD A is base register; <= max_reg
 *   UOPK_FRAME_REG_COUNT: OP_PUSH_FRAME_GUARD B is count; A+B <= max_reg+1
 *
 * UBxKind — how the verifier interprets the Bx field of UOPF_ABX opcodes:
 *   UBXK_UNUSED        : Bx ignored
 *   UBXK_POOL_INDEX    : OP_LOADK Bx must be < const_count
 *   UBXK_NESTED_INDEX  : OP_CLOSURE Bx must be < nested_count
 *   UBXK_JUMP_SIGNED   : OP_JMP Bx is biased signed offset; no range check
 *                        (target out-of-range surfaces at runtime)
 *   UBXK_HANDLER_PC    : OP_TRY_BEGIN / OP_PUSH_TAG handler/onleave PC;
 *                        must be < instr_count
 *   UBXK_SYMBOL_ID     : OP_LOAD_REALM_GLOBAL packs a 16-bit symbol id.
 *                        At v1.5 we accept the full 0..65535 range; the
 *                        runtime resolves against the realm's symbol
 *                        table at OP_LOAD_REALM_GLOBAL dispatch time and
 *                        reports unknown symbols there.
 */

#ifndef URBI_UOPCODE_SHAPE_H
#define URBI_UOPCODE_SHAPE_H

#include "chunk/uchunk.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UOPF_ABC = 0,
    UOPF_ABX = 1
} UOpcodeFormat;

typedef enum {
    UOPK_UNUSED = 0,
    UOPK_REG,
    UOPK_IMM_BOOL,
    UOPK_IMM_FLAGS,
    UOPK_IMM_REG_NIBBLE,
    UOPK_UPVAL_IDX,
    UOPK_FRAME_REG_BASE,
    UOPK_FRAME_REG_COUNT
} UOperandKind;

typedef enum {
    UBXK_UNUSED = 0,
    UBXK_POOL_INDEX,
    UBXK_NESTED_INDEX,
    UBXK_JUMP_SIGNED,
    UBXK_HANDLER_PC,
    UBXK_SYMBOL_ID
} UBxKind;

typedef struct {
    UOpcodeFormat format;
    UOperandKind  a_kind;
    UOperandKind  b_kind;
    UOperandKind  c_kind;
    UBxKind       bx_kind;
} UOpcodeShape;

/* Indexed by UOpcode value 0..OP_MAX-1.  Add new entries when
 * introducing new opcodes; the verifier consumes this table directly. */
extern const UOpcodeShape urbi_opcode_shapes[OP_MAX];

#endif /* URBI_UOPCODE_SHAPE_H */
