/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_unwind.c — unwind / control-transfer bytecode emitters.
 * Extracted from uemit.c during v0.5.4-decompose (EMIT-045 #6).
 *
 * Contains:
 *   - Public encoder helpers for unwind opcodes (uemit_throw, uemit_try_begin,
 *     uemit_try_end, uemit_tag_stop, uemit_push_tag, uemit_pop_tag,
 *     uemit_push_frame_guard, uemit_resume, uemit_load_catch_value).
 *   - emit_expr arm helpers for AST_THROW, AST_TRY, AST_TAG_PREFIX. */

#include "emit/uemit_internal.h"
#include "runtime/ucleanup.h"   /* FLAG_HAS_CATCH, FLAG_HAS_FINALLY */

/* =========================================================================
 * M3 row 7 control-transfer opcode encoder helpers.
 * Each function encodes exactly one instruction word and calls emit_instr.
 * See umodule.h §M3 row 7 for the field layout of each opcode.
 * ========================================================================= */

/* OP_THROW ABx: A = reg_value, Bx = 0 (unused). */
void uemit_throw(UEmitter *e, uint8_t reg_value, uint32_t line) {
    emit_instr(e, uinstr_enc_abx(OP_THROW, reg_value, 0u), line);
}

/* OP_TAG_STOP ABC: A = reg_tag, B = reg_value, C = 0. */
void uemit_tag_stop(UEmitter *e, uint8_t reg_tag, uint8_t reg_value, uint32_t line) {
    emit_instr(e, uinstr_enc_abc(OP_TAG_STOP, reg_tag, reg_value, 0u), line);
}

/* OP_TRY_BEGIN ABx: A = flags byte, Bx = handler PC (16-bit, range 0-65535).
 * flags bits: bit 0 = has_catch, bit 1 = has_finally (defined by T9/T10). */
void uemit_try_begin(UEmitter *e, uint8_t flags, uint16_t handler_pc, uint32_t line) {
    emit_instr(e, uinstr_enc_abx(OP_TRY_BEGIN, flags, handler_pc), line);
}

/* OP_TRY_END ABC: no operands (all zero). Pops top cleanup entry. */
void uemit_try_end(UEmitter *e, uint32_t line) {
    emit_instr(e, uinstr_enc_abc(OP_TRY_END, 0u, 0u, 0u), line);
}

/* OP_PUSH_TAG ABx: A packs flags nibble and tag_reg nibble.
 *   A[7:4] = flags (4 bits, values 0-15)
 *   A[3:0] = tag_reg (4 bits, values 0-15)
 *   Bx     = onleave PC (16-bit, range 0-65535)
 * tag_reg must be in [0,15]; flags must be in [0,15]. T30 revisits
 * if wider operand ranges become necessary. */
void uemit_push_tag(UEmitter *e, uint8_t reg_tag, uint8_t flags,
                    uint16_t onleave_pc, uint32_t line) {
    uint8_t a = (uint8_t)(((flags & 0xFu) << 4) | (reg_tag & 0xFu));
    emit_instr(e, uinstr_enc_abx(OP_PUSH_TAG, a, onleave_pc), line);
}

/* OP_POP_TAG ABC: A = reg_tag, B = C = 0. */
void uemit_pop_tag(UEmitter *e, uint8_t reg_tag, uint32_t line) {
    emit_instr(e, uinstr_enc_abc(OP_POP_TAG, reg_tag, 0u, 0u), line);
}

/* OP_PUSH_FRAME_GUARD ABC: A = register_base, B = register_count, C = 0. */
void uemit_push_frame_guard(UEmitter *e, uint8_t register_base,
                             uint8_t register_count, uint32_t line) {
    emit_instr(e, uinstr_enc_abc(OP_PUSH_FRAME_GUARD, register_base,
                                  register_count, 0u), line);
}

/* OP_RESUME ABC: A = reg_state, B = C = 0. */
void uemit_resume(UEmitter *e, uint8_t reg_state, uint32_t line) {
    emit_instr(e, uinstr_enc_abc(OP_RESUME, reg_state, 0u, 0u), line);
}

/* OP_LOAD_CATCH_VALUE ABC: A = destination register, B = C = 0.
 * T10 empirical addition — loads s->catch_value into R[A] at handler entry. */
void uemit_load_catch_value(UEmitter *e, uint8_t reg, uint32_t line) {
    emit_instr(e, uinstr_enc_abc(OP_LOAD_CATCH_VALUE, reg, 0u, 0u), line);
}
