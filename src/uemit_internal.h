/* SPDX-License-Identifier: BSD-3-Clause */
/* Internal — exposes UFuncState to unit tests. NOT part of the public
 * API. Not installed; not for downstream consumers. */

#ifndef UEMIT_INTERNAL_H
#define UEMIT_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UFS_MAX_LOCALS    200       /* per-function lexical-local cap */
#define UFS_MAX_UPVALUES   60       /* per-function upvalue cap (T8) */
#define UFS_MAX_BLOCKS     32       /* per-function block-nesting cap (T7) */
#define UFS_MAX_REGS      256       /* per-function register-frame cap */

/* Active-local descriptor. Lifetime = lexical scope; popped on block exit
 * or function close (T7 adds the block-pop semantics). The slot index
 * equals the register holding the local's value (registers [0, nactvar)
 * are locals; [nactvar, freereg) are temps; [freereg, UFS_MAX_REGS) are
 * free). */
typedef struct {
    const char *name;                /* canonical (interned) pointer */
    int         name_len;
    uint8_t     slot;                /* register holding the value */
    bool        is_captured;         /* true once any inner closure flagged
                                        it as an upvalue source — T8 sets;
                                        T7 reads to drive OP_CLOSE. */
    bool        is_lazy;             /* T16: lazy parameter; reserved here */
} ULocalVar;

/* Upvalue descriptor — one per captured outer local. Reserved at T6;
 * populated at T8. */
typedef struct {
    const char *name;                /* interned; for diagnostics + closure prelude */
    int         name_len;
    uint8_t     idx;                 /* if in_stack: enclosing local slot;
                                        else: enclosing upvalue index */
    bool        in_stack;            /* true: capture from immediate parent's
                                        locals; false: re-capture parent's upvalue */
} UUpvalDesc;

/* Block context — stacked per `{` / function-body / loop body. Reserved
 * at T6; populated at T7. */
typedef struct {
    int    nactvar_on_enter;         /* nactvar value when block opened */
    int    first_local_idx;          /* index into actvars[] of first local
                                        declared in this block */
    bool   is_loop;                  /* true for while-body block —
                                        drives OP_CLOSE-before-back-edge (T8) */
    bool   has_captured;             /* set when any local in this block
                                        was flagged is_captured */
    int    break_chain;              /* head of OP_JMP fixup chain for
                                        break (M2.5; reserved) */
    int    continue_chain;           /* head of OP_JMP fixup chain for
                                        continue (M2.5; reserved) */
} UBlockCtx;

/* Per-function compiler state. Arena-allocated by uemit_open_function;
 * destroyed implicitly by arena reset. */
typedef struct UFuncState {
    struct UFuncState *parent;       /* enclosing function (NULL at top level) */

    /* Register accounting. */
    uint8_t freereg;                 /* cursor — next free register */
    uint8_t max_reg_seen;            /* high-water; becomes proto->max_reg */
    int     nactvar;                 /* count of currently-active lexical locals */

    /* Lexical-local stack (LIFO; pushed by uemit_declare_local). */
    ULocalVar actvars[UFS_MAX_LOCALS];

    /* Upvalue descriptors (T8 populates). */
    UUpvalDesc upvalues[UFS_MAX_UPVALUES];
    int        nupvalues;

    /* Block stack (T7 populates). */
    UBlockCtx blocks[UFS_MAX_BLOCKS];
    int       nblocks;

    /* Where this function's bytecode lives. At top-level, this is
     * the emitter's main module. For nested protos (T14), this is the
     * nested UProto's instruction buffer. NULL at T6 — top-level only. */
    void *target_proto;              /* opaque; cast to UProto* in T14 */
} UFuncState;

/* Compile-time upvalue cascade. Walks parent FuncStates to find `name`
 * (must be interned) and installs it as an upvalue in fs's table.
 * Returns the upvalue index in fs, or -1 if name is not found in any
 * enclosing scope. Sets EMIT_UPVAL_EXHAUSTED on overflow. */
int find_or_install_upvalue(struct UEmitter *e, struct UFuncState *fs,
                            const char *name, int name_len);

#ifdef __cplusplus
}
#endif

#endif /* UEMIT_INTERNAL_H */
