/* SPDX-License-Identifier: BSD-3-Clause */
/* Single-pass bytecode emitter.  AST -> UModule.  Hosted. */

#ifndef UEMIT_H
#define UEMIT_H

#include <stdarg.h>               /* va_list — emit_diag_warn variadic */
#include <stdbool.h>
#include <stddef.h>               /* ptrdiff_t */
#include <stdint.h>

#include "value/uarena.h"
#include "parse/uast.h"
#include "chunk/umodule.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- emit-time diagnostic (warn/error) plumbing (T32) --- */

typedef struct {
    enum { UEMIT_DIAG_WARN = 0, UEMIT_DIAG_ERROR = 1 } level;
    int         line;
    int         col;
    const char *message;    /* allocator-owned copy; freed by emit_diag_free_all */
} UEmitDiag;

/* --- emit-time errors (distinct from loader errors) --- */

typedef enum {
    EMIT_OK = 0,
    EMIT_OOM,                     /* module buffer grow failed */
    EMIT_AST_ERROR,               /* input AST contained AST_ERROR */
    EMIT_UNSUPPORTED_AST,         /* AST kind not emittable at this milestone */
    EMIT_REG_EXHAUSTED,           /* > 255 registers needed — deep expression */
    EMIT_CONSTANT_POOL_FULL,      /* > 65535 constants — Bx overflow */
    EMIT_LINE_OVERFLOW,           /* source line > UINT32_MAX (effectively unreachable) */
    EMIT_FINISHED,                /* uemit_statement called after uemit_finish */

    /* M2 additions */
    EMIT_UPVAL_EXHAUSTED,         /* > UFS_MAX_UPVALUES captures (T8) */
    EMIT_LOCAL_REDECLARE,         /* duplicate `var x` in same block */
    EMIT_UNRESOLVED_NAME,         /* identifier not local/upvalue/global */
    EMIT_NESTING_TOO_DEEP,        /* > UFS_MAX_BLOCKS or function-nesting cap (T7) */
    EMIT_BARE_LAZY_FUNCTION,      /* T17: `function name { body }` */
    EMIT_CLOSURE_KEYWORD,         /* T17: `closure(x){...}` */
    EMIT_LAZY_ON_METHOD,          /* T16: lazy on method-bound function */
    EMIT_LAZY_PARAM_ASSIGN,       /* T16: assignment to lazy param */

    /* M4 additions */
    EMIT_TOO_MANY_IC_SITES,       /* T15: function exceeds 256 IC sites
                                     (pre-M4 GETSLOT/SETSLOT encoding §3.4) */

    /* M5 additions */
    EMIT_RESERVED_KEYWORD_AS_IDENT, /* T4: `var at = 1` — hard keyword as variable name */

    /* v0.5.7 Wave 5 additions */
    EMIT_TOO_MANY_ARGS,             /* EMIT-014: AST_CALL with >= 254 args
                                       (B field encodes nargs+1 as uint8_t,
                                       wraps at 256) */
    EMIT_TAG_SPILL_OUT_OF_RANGE,    /* EMIT-015: AST_TAG_PREFIX spill register
                                       does not fit OP_PUSH_TAG's 4-bit
                                       reg-nibble.  v1.x bytecode change widens
                                       the encoding (filed as backlog under
                                       T129/Phase 22). */

    /* v0.6.2 Phase 2 — Gap #3 (this keyword) */
    EMIT_NO_THIS_OUTSIDE_METHOD     /* `this` used at top-level (fs->parent ==
                                       NULL).  Top-level `this` resolves to the
                                       lobby object — deferred to v1.x
                                       (REVIVAL §14 S29). */
} UEmitError;

/* Forward declaration for M2 FuncState lifecycle. */
struct UFuncState;

/* --- UEmitter state (caller stack-allocates, emitter fills) --- */

typedef struct UEmitter {
    UModule       *module;           /* non-owning; caller supplies */
    UArena       *arena;           /* non-owning; currently unused at M1 but reserved */
    struct UVM   *vm;              /* non-owning; set by uemit_init (M2) for intern access */
    uint8_t      next_reg;        /* register allocator cursor */
    uint8_t      max_reg_seen;    /* highest slot ever used */
    uint8_t      last_result_reg; /* register of most recent statement's result */
    uint32_t     prev_line;       /* last emitted instruction's source line */
    bool         any_stmt_emitted;/* gates OP_RET at finish */
    bool         finished;
    bool         lazy_arg_context; /* T16: set while emitting args in AST_CALL;
                                      suppresses implicit force on lazy-local reads
                                      (pass-through semantics, spec §4.2) */
    UEmitError    error;           /* sticky: first error latches */
    struct UFuncState *current_fs; /* M2: current compilation function */

    /* T32: warn-level diagnostic buffer.  emit_diag_warn appends here;
     * never causes emit to fail.  diag_buf is module-allocator-owned and
     * grows by doubling.  diag_count diagnostics are valid after
     * uemit_finish; callers may walk diag_buf[0..diag_count-1]. */
    UEmitDiag   *diag_buf;
    int          diag_count;
    int          diag_cap;
} UEmitter;

/* --- API --- */

/* Initialize.  module, arena, and vm must all outlive the emitter.
   source_name may be NULL.  vm parameter (added at M2) lets the
   emitter intern identifier lexemes into the per-VM string table and
   stamps module->origin_vm = vm. */
void uemit_init(UEmitter *e, UModule *module, UArena *arena,
                struct UVM *vm, const char *source_name);

/* Emit one statement's bytecode into the module.  stmt must be non-NULL.
   AST_ERROR nodes are rejected with EMIT_AST_ERROR.  On first error, the
   error latches; subsequent calls return it without touching the module. */
UEmitError uemit_statement(UEmitter *e, UAstNode *stmt);

/* Finalize: emit OP_RET (if any statement was emitted) and record max_reg.
   Further uemit_statement calls return EMIT_FINISHED.  Returns the first
   accumulated error, or EMIT_OK. */
UEmitError uemit_finish(UEmitter *e);

/* Open a new compilation function. At top-level, parent==NULL. Returns
   NULL on OOM (sets EMIT_OOM). The opened FuncState becomes
   e->current_fs. */
struct UFuncState *uemit_open_function(UEmitter *e, struct UFuncState *parent);

/* Close the current function. Pops parent into e->current_fs. Returns
   the closed FuncState* (still arena-valid; caller may inspect). */
struct UFuncState *uemit_close_function(UEmitter *e);

/* Declare a local in the current function. name must be interned
   (canonical pointer; pointer-eq for redeclare detection). Returns the
   slot index, or -1 on EMIT_LOCAL_REDECLARE / EMIT_REG_EXHAUSTED (sets
   e->error). */
int uemit_declare_local(UEmitter *e, const char *name, int name_len);

/* Open a lexical block scope. Pushes a UBlockCtx with current
   nactvar/freereg snapshot. is_loop=true marks while-bodies for
   T8's OP_CLOSE-before-back-edge rule. Returns true on success;
   false (with EMIT_NESTING_TOO_DEEP) if UFS_MAX_BLOCKS exceeded. */
bool uemit_open_block(UEmitter *e, bool is_loop);

/* Close the topmost block. Emits OP_CLOSE if any local in this block
   was captured. Restores freereg to the pre-open value; pops actvars
   back to nactvar_on_enter. Returns true on success. */
bool uemit_close_block(UEmitter *e);

/* For T13 while-loop emit: emit OP_CLOSE before the back-edge JMP when
 * the topmost block is a loop AND has captured locals (Lua-style
 * closure-in-loop correctness). No-op otherwise. */
void uemit_emit_loop_back_close(UEmitter *e);

/* Debug helper. */
const char *uemit_error_name(UEmitError code);

/* T32: Append a warn-level diagnostic to the emitter's diag buffer.
 * n may be NULL (position will be 0,0).  fmt is a printf-style format
 * string.  Does not set e->error; emit continues normally.
 * If the buffer cannot grow (OOM), the diagnostic is silently dropped. */
void emit_diag_warn(UEmitter *e, UAstNode *n, const char *fmt, ...);

/* T32: Free all diagnostic message strings and the diag_buf array itself.
 * Resets diag_count/diag_cap to 0.  Must be called before the emitter's
 * associated module is destroyed.  No-op on freestanding builds. */
void emit_diag_free_all(UEmitter *e);

/* --- M3 row 7 control-transfer opcode encoder helpers ---
 *
 * These emit exactly one instruction word into the module.  All accept the
 * source line number for syncline tracking.  See umodule.h §M3 row 7 for
 * the bit-layout of each opcode's fields.
 *
 * Used by T10 (try/catch/throw emit), T11 (tag-scope emit), and tests. */

/* OP_THROW: reg_value is the register holding the thrown value. */
void uemit_throw(UEmitter *e, uint8_t reg_value, uint32_t line);

/* OP_TAG_STOP: reg_tag is the tag register, reg_value is the stop-value register. */
void uemit_tag_stop(UEmitter *e, uint8_t reg_tag, uint8_t reg_value, uint32_t line);

/* OP_TRY_BEGIN: flags (bit 0=has_catch, bit 1=has_finally); handler_pc is the
 * PC of the catch/finally handler (16-bit, 0-65535 instruction words). */
void uemit_try_begin(UEmitter *e, uint8_t flags, uint16_t handler_pc, uint32_t line);

/* OP_TRY_END: no operands; pops the top cleanup-stack entry. */
void uemit_try_end(UEmitter *e, uint32_t line);

/* OP_PUSH_TAG: reg_tag in [0,15], flags in [0,15] (4-bit fields packed into A);
 * onleave_pc is the PC of the on-leave handler (16-bit). */
void uemit_push_tag(UEmitter *e, uint8_t reg_tag, uint8_t flags,
                    uint16_t onleave_pc, uint32_t line);

/* OP_POP_TAG: reg_tag is the tag register to pop. */
void uemit_pop_tag(UEmitter *e, uint8_t reg_tag, uint32_t line);

/* OP_PUSH_FRAME_GUARD: register_base and register_count define the guarded range. */
void uemit_push_frame_guard(UEmitter *e, uint8_t register_base,
                             uint8_t register_count, uint32_t line);

/* OP_RESUME: reg_state is the register holding the saved unwind state. */
void uemit_resume(UEmitter *e, uint8_t reg_state, uint32_t line);

/* OP_LOAD_CATCH_VALUE: reg is the destination register for the caught value.
 * Emitted as the first instruction of every catch-handler body; the unwind
 * walker writes s->catch_value before jumping to the handler PC. */
void uemit_load_catch_value(UEmitter *e, uint8_t reg, uint32_t line);

/* Write a human-readable disassembly of the module into buf.
   Returns bytes written (excluding null terminator).  Truncates if cap is
   too small; always null-terminates when cap > 0.
   Format: one instruction per line, e.g. "LOADK R0, K0".  Trailing
   constant-pool dump. */
size_t uemit_disassemble(const UModule *module, char *buf, size_t cap);

/* Serialize module to the .urb byte format.
   Returns bytes written on success (including the case where buf == NULL
   and cap == 0 — first-pass size query).
   Returns a negative value on failure: -(ptrdiff_t)UModuleLoadError code. */
ptrdiff_t umodule_serialize(const UModule *module, uint8_t *buf, size_t cap);

/* --- Test-friend internals ---
 * Originally exposed via uemit_internal.h to allow unit tests
 * (tests/unit/test_funcstate.c) to access UFuncState directly.  Now
 * lives here since src/ is internal-by-definition post-include/-split.
 * Embedders never include uemit.h — only urbi.h is public. */

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
    int     nactvar_on_enter;        /* nactvar value when block opened */
    uint8_t freereg_on_enter;        /* freereg value when block opened;
                                        used by uemit_close_block to restore
                                        the temp-zone floor correctly (needed
                                        when r_global_slot is pre-reserved and
                                        `freereg != nactvar` invariant is broken). */
    int    first_local_idx;          /* index into actvars[] of first local
                                        declared in this block */
    bool   is_loop;                  /* true for while-body block —
                                        drives OP_CLOSE-before-back-edge (T8) */
    bool   has_captured;             /* set when any local in this block
                                        was flagged is_captured */
} UBlockCtx;

/* Per-local function-signature metadata (T16: lazy parameter tracking).
 * Populated when a var-decl init is a literal AST_FUNCTION; stays
 * resolved=false for all other binding forms. */
typedef struct {
    int  nparams;
    bool param_is_lazy[16];      /* per-param flag; cap 16 */
    bool resolved;               /* false if local doesn't hold a known function */
} UFuncSig;

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

    /* Per-local function-signature metadata (T16). Parallel to actvars[].
     * Populated when var-decl init is a literal AST_FUNCTION. */
    UFuncSig actvar_sigs[UFS_MAX_LOCALS];

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

    /* === M4 T15: per-function IC site bookkeeping === */
    /* Per pre-M4 GETSLOT/SETSLOT encoding spec §3.4 + §8.2.  Each
     * OP_GETSLOT/SETSLOT instruction in this function carries an 8-bit IC
     * index; ic_next is the next index to assign.  ic_names is a dynamic
     * array (grown in 16/32/64/128/256-slot chunks) sized to ic_names_cap;
     * at uemit_close_function, the populated [0, ic_next) prefix is copied
     * into target_proto->ic_names and ic_count.  256-cap raises
     * EMIT_TOO_MANY_IC_SITES. */
    uint16_t   ic_next;              /* equals proto->ic_count after close */
    USymbol  **ic_names;             /* lazily allocated via module allocator */
    uint16_t   ic_names_cap;

    /* === M5 T71: realm-global fallback register ===
     *
     * When this function references any realm global (identifier that does
     * not resolve as a local or upvalue), references_global is set and
     * r_global_slot is assigned a register that will hold realm->global_object
     * at runtime.  T73 prepends OP_LOAD_REALM_GLOBAL r_global_slot to the
     * function prologue.  All global OP_GETSLOT(dst, r_global_slot, ic_idx)
     * instructions in this function route through that single live register.
     *
     * r_global_slot is claimed from freereg (same floor as local slots) so
     * it stays valid across statement boundaries; freereg is bumped to prevent
     * the temp zone from aliasing it.
     *
     * EMIT-021 state machine — global_slot_reserved and references_global are
     * NOT synonyms.  They encode three distinct states:
     *
     *   (1) UNUSED          : !global_slot_reserved && !references_global
     *       Nested funcstate that has not yet been emitted via
     *       emit_function_literal (no slot claim, no global reads).
     *
     *   (2) RESERVED_NO_REF :  global_slot_reserved && !references_global
     *       Slot has been claimed from freereg (and floor includes it) but
     *       no global identifier has resolved yet.  Two ways to reach this
     *       state:
     *         - chunk-top funcstate at uemit_open_function entry: the slot
     *           is unconditionally pre-reserved so a subsequent if/while
     *           condition register cannot collide at index 0;
     *         - nested function body in emit_function_literal: pre-reserved
     *           above the last param so an if-arm-only first global use
     *           still claims a stable slot.
     *
     *   (3) REFERENCED      :  global_slot_reserved &&  references_global
     *       After first global identifier resolves; uemit_close_function
     *       emits OP_LOAD_REALM_GLOBAL as a function prologue.
     *
     * The fourth combination (!reserved && referenced) is unreachable —
     * resolving a global always claims the slot first.
     *
     * Floor semantics: fs_temp_floor includes the global slot iff
     * global_slot_reserved (NOT iff references_global).  This is the
     * load-bearing distinction: a chunk-top with no global reads still
     * has its r_global_slot register protected from temp-zone overwrites,
     * while close still skips the prologue (no wasted instruction).
     *
     * Regression test: tests/unit/test_emit_global_lookup.c
     *   emit_global_state_machine_distinct_flags (RESERVED_NO_REF)
     *   emit_global_state_machine_advances_to_referenced (REFERENCED)
     *   emit_global_pure_local_chunk_no_prologue (RESERVED_NO_REF stays
     *     RESERVED_NO_REF when no global is read — no prologue emitted) */
    bool     references_global;      /* true after first global ident resolved;
                                        gates OP_LOAD_REALM_GLOBAL prologue
                                        emission in uemit_close_function. */
    bool     global_slot_reserved;   /* true once r_global_slot is claimed from
                                        freereg; gates fs_temp_floor inclusion.
                                        Set INDEPENDENTLY of references_global
                                        — see EMIT-021 state machine above. */
    uint8_t  r_global_slot;          /* register for realm->global_object;
                                        meaningful only when
                                        global_slot_reserved is true. */

    /* === M5 T72: chunk-top declared global names ===
     *
     * At chunk-top (parent == NULL), `var x = init` does not add `x` to
     * actvars[] — it writes to the global slot via OP_SETSLOT.  To allow
     * subsequent `x = value` assignments (AST_ASSIGN) to route to the global
     * slot rather than raising EMIT_UNRESOLVED_NAME, the canonical name is
     * stored here at declaration time.  Names are interned pointers (same
     * intern table as actvars), so pointer equality suffices for lookup.
     *
     * Cap matches UFS_MAX_LOCALS (a chunk-top function can't declare more
     * globals than a nested function can declare locals, by the same reg
     * budget). */
    int          n_global_vars;
    const char  *global_var_names[UFS_MAX_LOCALS];
    UFuncSig     global_var_sigs[UFS_MAX_LOCALS]; /* parallel to global_var_names[];
                                                     resolved=true when the init is
                                                     a literal AST_FUNCTION, enabling
                                                     T16 lazy-arg wrapping at call sites
                                                     that reference globals. */
} UFuncState;

/* Compile-time upvalue cascade. Walks parent FuncStates to find `name`
 * (must be interned) and installs it as an upvalue in fs's table.
 * Returns the upvalue index in fs, or -1 if name is not found in any
 * enclosing scope. Sets EMIT_UPVAL_EXHAUSTED on overflow. */
int find_or_install_upvalue(struct UEmitter *e, struct UFuncState *fs,
                            const char *name, int name_len);

/* M4 T15: assign the next IC site index for the current function and
 * record `name` in the funcstate's ic_names side table.  `name` is the
 * (interned) USymbol* identifying the slot accessed by an OP_GETSLOT or
 * OP_SETSLOT instruction; the caller writes the returned index into the
 * instruction's C operand.  Returns the assigned index in [0, 256), or
 * -1 on error (sets e->error to EMIT_TOO_MANY_IC_SITES on overflow,
 * EMIT_OOM on growth-allocator failure).  Two calls for the same `name`
 * return distinct indices — every emitted GETSLOT/SETSLOT site gets its
 * own IC slot so per-site monomorphism is independent. */
int uemit_assign_ic_index(struct UEmitter *e, USymbol *name);

/* T31: Test-friend export — see uemit.c for full documentation.
 * Best-effort compile-time walker: returns true when n contains a direct
 * write (AST_ASSIGN, AST_VAR_DECL, AST_MEMBER_SET, AST_PROP_SET).
 * AST_CALL is treated as opaque (returns false). */
bool cond_has_direct_side_effect(UAstNode *n);

#ifdef __cplusplus
}
#endif

#endif
