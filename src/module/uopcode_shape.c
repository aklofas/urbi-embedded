/* SPDX-License-Identifier: BSD-3-Clause */
/* uopcode_shape.c — opcode-shape table data.  See uopcode_shape.h. */

#include "uopcode_shape.h"

const UOpcodeShape urbi_opcode_shapes[OP_MAX] = {
    /* M1 (v1.0) opcodes 0-7 */
    [OP_LOADK]    = { UOPF_ABX, UOPK_REG,    UOPK_UNUSED, UOPK_UNUSED, UBXK_POOL_INDEX },
    [OP_MOVE]     = { UOPF_ABC, UOPK_REG,    UOPK_REG,    UOPK_UNUSED, UBXK_UNUSED },
    [OP_ADD]      = { UOPF_ABC, UOPK_REG,    UOPK_REG,    UOPK_REG,    UBXK_UNUSED },
    [OP_SUB]      = { UOPF_ABC, UOPK_REG,    UOPK_REG,    UOPK_REG,    UBXK_UNUSED },
    [OP_MUL]      = { UOPF_ABC, UOPK_REG,    UOPK_REG,    UOPK_REG,    UBXK_UNUSED },
    [OP_DIV]      = { UOPF_ABC, UOPK_REG,    UOPK_REG,    UOPK_REG,    UBXK_UNUSED },
    [OP_NEG]      = { UOPF_ABC, UOPK_REG,    UOPK_REG,    UOPK_UNUSED, UBXK_UNUSED },
    [OP_RET]      = { UOPF_ABC, UOPK_REG,    UOPK_UNUSED, UOPK_UNUSED, UBXK_UNUSED },

    /* M2 (v1.1) opcodes 8-23 */
    [OP_LOADNIL]  = { UOPF_ABC, UOPK_REG,        UOPK_UNUSED,    UOPK_UNUSED, UBXK_UNUSED },
    [OP_LOADBOOL] = { UOPF_ABC, UOPK_REG,        UOPK_IMM_BOOL,  UOPK_IMM_BOOL, UBXK_UNUSED },
    [OP_LOADVOID] = { UOPF_ABC, UOPK_REG,        UOPK_UNUSED,    UOPK_UNUSED, UBXK_UNUSED },
    [OP_GETUPVAL] = { UOPF_ABC, UOPK_REG,        UOPK_UPVAL_IDX, UOPK_UNUSED, UBXK_UNUSED },
    [OP_SETUPVAL] = { UOPF_ABC, UOPK_REG,        UOPK_UPVAL_IDX, UOPK_UNUSED, UBXK_UNUSED },
    /* OP_CLOSURE: at v1.5 the NUP upvalue-descriptor pseudo-instructions
     * following the OP_CLOSURE are not verified at load time — runtime
     * dispatch consumes them.  v1.x backlog: extend the verifier to walk
     * the prelude. */
    [OP_CLOSURE]  = { UOPF_ABX, UOPK_REG,        UOPK_UNUSED,    UOPK_UNUSED, UBXK_NESTED_INDEX },
    [OP_CLOSE]    = { UOPF_ABC, UOPK_REG,        UOPK_UNUSED,    UOPK_UNUSED, UBXK_UNUSED },
    [OP_CALL]     = { UOPF_ABC, UOPK_REG,        UOPK_REG,       UOPK_REG,    UBXK_UNUSED },
    [OP_JMP]      = { UOPF_ABX, UOPK_UNUSED,     UOPK_UNUSED,    UOPK_UNUSED, UBXK_JUMP_SIGNED },
    [OP_TEST]     = { UOPF_ABC, UOPK_REG,        UOPK_UNUSED,    UOPK_IMM_BOOL, UBXK_UNUSED },
    [OP_TESTSET]  = { UOPF_ABC, UOPK_REG,        UOPK_REG,       UOPK_IMM_BOOL, UBXK_UNUSED },
    [OP_EQ]       = { UOPF_ABC, UOPK_IMM_BOOL,   UOPK_REG,       UOPK_REG,    UBXK_UNUSED },
    [OP_NEQ]      = { UOPF_ABC, UOPK_IMM_BOOL,   UOPK_REG,       UOPK_REG,    UBXK_UNUSED },
    [OP_LT]       = { UOPF_ABC, UOPK_IMM_BOOL,   UOPK_REG,       UOPK_REG,    UBXK_UNUSED },
    [OP_LE]       = { UOPF_ABC, UOPK_IMM_BOOL,   UOPK_REG,       UOPK_REG,    UBXK_UNUSED },
    [OP_YIELD]    = { UOPF_ABC, UOPK_UNUSED,     UOPK_UNUSED,    UOPK_UNUSED, UBXK_UNUSED },

    /* M3 row 7 separator opcodes 24-26 */
    [OP_FORK_DETACH] = { UOPF_ABC, UOPK_REG,    UOPK_UNUSED, UOPK_UNUSED, UBXK_UNUSED },
    [OP_FORK_JOIN]   = { UOPF_ABC, UOPK_REG,    UOPK_UNUSED, UOPK_UNUSED, UBXK_UNUSED },
    [OP_JOIN_WAIT]   = { UOPF_ABC, UOPK_REG,    UOPK_UNUSED, UOPK_UNUSED, UBXK_UNUSED },

    /* M4 GETSLOT/SETSLOT 27-28 — A=dst/value reg, B=recv reg, C=ic-site index (uint8) */
    [OP_GETSLOT]  = { UOPF_ABC, UOPK_REG,        UOPK_REG,       UOPK_UNUSED, UBXK_UNUSED },
    [OP_SETSLOT]  = { UOPF_ABC, UOPK_REG,        UOPK_REG,       UOPK_UNUSED, UBXK_UNUSED },

    /* M3 row 7 control transfer 29-37 */
    [OP_THROW]              = { UOPF_ABC, UOPK_REG,                UOPK_UNUSED,        UOPK_UNUSED, UBXK_UNUSED },
    [OP_TAG_STOP]           = { UOPF_ABC, UOPK_REG,                UOPK_REG,           UOPK_UNUSED, UBXK_UNUSED },
    [OP_TRY_BEGIN]          = { UOPF_ABX, UOPK_IMM_FLAGS,          UOPK_UNUSED,        UOPK_UNUSED, UBXK_HANDLER_PC },
    [OP_TRY_END]            = { UOPF_ABC, UOPK_UNUSED,             UOPK_UNUSED,        UOPK_UNUSED, UBXK_UNUSED },
    [OP_PUSH_TAG]           = { UOPF_ABX, UOPK_IMM_REG_NIBBLE,     UOPK_UNUSED,        UOPK_UNUSED, UBXK_HANDLER_PC },
    [OP_POP_TAG]            = { UOPF_ABC, UOPK_REG,                UOPK_UNUSED,        UOPK_UNUSED, UBXK_UNUSED },
    [OP_PUSH_FRAME_GUARD]   = { UOPF_ABC, UOPK_FRAME_REG_BASE,     UOPK_FRAME_REG_COUNT, UOPK_UNUSED, UBXK_UNUSED },
    [OP_RESUME]             = { UOPF_ABC, UOPK_REG,                UOPK_UNUSED,        UOPK_UNUSED, UBXK_UNUSED },
    [OP_LOAD_CATCH_VALUE]   = { UOPF_ABC, UOPK_REG,                UOPK_UNUSED,        UOPK_UNUSED, UBXK_UNUSED },

    /* M5 reactive 38-45 (renumbered at v0.5.6 T17; v1.4 was 39-46).
     * Install ops: C carries either an onleave-closure register OR the
     * 0xFF "no onleave" sentinel; UOPK_UNUSED so the verifier accepts
     * arbitrary byte values (runtime decodes the sentinel at dispatch).
     * OP_GETSLOT_CHANGE_EVENT C is the IC site index, matching
     * OP_GETSLOT / OP_SETSLOT. */
    [OP_AT_INSTALL]            = { UOPF_ABC, UOPK_REG, UOPK_REG, UOPK_UNUSED, UBXK_UNUSED },
    [OP_AT_SYNC_INSTALL]       = { UOPF_ABC, UOPK_REG, UOPK_REG, UOPK_UNUSED, UBXK_UNUSED },
    [OP_WHENEVER_INSTALL]      = { UOPF_ABC, UOPK_REG, UOPK_REG, UOPK_UNUSED, UBXK_UNUSED },
    [OP_WAITUNTIL_INSTALL]     = { UOPF_ABC, UOPK_REG, UOPK_UNUSED, UOPK_UNUSED, UBXK_UNUSED },
    [OP_AT_EVENT_INSTALL]      = { UOPF_ABC, UOPK_REG, UOPK_REG, UOPK_UNUSED, UBXK_UNUSED },
    [OP_AT_EVENT_SYNC_INSTALL] = { UOPF_ABC, UOPK_REG, UOPK_REG, UOPK_UNUSED, UBXK_UNUSED },
    [OP_GETSLOT_CHANGE_EVENT]  = { UOPF_ABC, UOPK_REG, UOPK_REG, UOPK_UNUSED, UBXK_UNUSED },
    /* OP_LOAD_REALM_GLOBAL at v1.5: emitter writes ABC with B=C=0; only A
     * (dst_reg) is consumed by the VM.  The forward-looking ABX/sym_id
     * shape extension is deferred — needs a concrete realm symbol-table
     * layout (see backlog). */
    [OP_LOAD_REALM_GLOBAL]     = { UOPF_ABC, UOPK_REG, UOPK_UNUSED, UOPK_UNUSED, UBXK_UNUSED },
};
