/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_internal.h — the emitter's state and its inter-TU helpers.
 *
 * Consumed only by src/emit/ TUs.  The public emit API is in uemit.h.
 *
 * Register discipline, in one paragraph.  Each function state has one
 * cursor, `freereg`.  Locals -- named variables and nameless pins -- are
 * declared at `freereg` and own their register until their scope ends;
 * the newest local's register plus one is `ureg_top`.  Everything from
 * `ureg_top` up to `freereg` is the temporaries of the statement being
 * compiled, and every statement ends with `freereg == ureg_top`.  A value
 * that has to survive a nested statement is pinned (`upin`), which makes
 * it a nameless local, so the nested statement's own resets land above
 * it without any arithmetic. */

#ifndef UEMIT_INTERNAL_H
#define UEMIT_INTERNAL_H

#include "uemit.h"
#include "util/umacros.h"   /* urbi_strlen, urbi_zero */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Local byte-copy, so the emitter needs no hosted <string.h>. */
static inline void emit_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *pd = (unsigned char *)dst;
    const unsigned char *ps = (const unsigned char *)src;
    size_t i;
    for (i = 0U; i < n; i++) pd[i] = ps[i];
}

/* --- Module allocator helper --- */

#if __STDC_HOSTED__
#  include <stdlib.h>

static inline void *emit_stdlib_alloc(void *ptr, size_t nbytes, void *ud) {
    (void)ud;
    if (nbytes == 0U) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}

#endif  /* __STDC_HOSTED__ */

/* Return the allocator to use for root UProto c.  In freestanding builds
   the stdlib fallback is absent and the caller must have supplied
   alloc_fn. */
static inline UChunkAllocFn emit_alloc_for(const UProto *c) {
#if __STDC_HOSTED__
    return c->alloc_fn != NULL ? c->alloc_fn : emit_stdlib_alloc;
#else
    return c->alloc_fn;   /* freestanding: caller must supply */
#endif
}

/* --- caps --- */

#define UFS_MAX_LOCALS    200
#define UFS_MAX_UPVALUES   60
#define UFS_MAX_BLOCKS     32
#define UFS_MAX_REGS      256
#define UEMIT_LOOP_MAX      8
#define UEMIT_PATCH_MAX    16
#define UEMIT_SCOPE_MAX    16
/* Chunk-top bindings whose initializer was a function with a lazy
 * parameter; a call through one of them wraps the flagged arguments. */
#define UEMIT_LAZY_GLOBALS_MAX 64
/* OP_JMP's Bx is a signed offset biased by this much, so Bx 0x0000 means
   "jump back 0x8000" and 0x8000 means "offset 0". */
#define UEMIT_JMP_BIAS  32768

/* --- function state --- */

typedef struct {
    const char *name;        /* interned; NULL for a pinned temporary */
    uint8_t     reg;
    uint8_t     flags;       /* ULOCAL_* */
    uint16_t    lazy_mask;   /* bit i set: parameter i of the function this binding holds is lazy; 0 when unknown */
} ULocal;
#define ULOCAL_CAPTURED    0x1
#define ULOCAL_GLOBAL_DECL 0x2   /* chunk-top `var`: lives in the globals object, `reg` is unused */
#define ULOCAL_LAZY_PARAM  0x4   /* reading it forces the thunk unless in a lazy-arg position */
#define ULOCAL_PINNED      0x8

typedef struct { const char *name; uint8_t idx; bool in_stack; } UUpval;
/* `freereg_on_enter` is both what the close restores and the CLOSE
 * operand: every local the block declared sits at or above it. */
typedef struct { int nactvar_on_enter; uint8_t freereg_on_enter; bool is_loop; bool has_captured; } UBlock;
/* A JMP's Bx is relative; an UNWIND_TO's is absolute.  `is_unwind`
 * tells the patch which one it is writing. */
typedef struct { int pcs[UEMIT_PATCH_MAX]; uint8_t is_unwind[UEMIT_PATCH_MAX]; int count; } UPatchList;
typedef struct { UPatchList breaks, continues; bool is_switch; int scope_depth_on_enter; } ULoop;
/* One record per cleanup-stack entry the frame has open at the point
 * being compiled.  A finally body counts as one: it runs under the
 * walker's RUNNING marker. */
typedef struct { uint8_t kind; /* UEMIT_SCOPE_* */ uint8_t tag_reg; } UScope;
#define UEMIT_SCOPE_TRY     1U
#define UEMIT_SCOPE_TAG     2U
#define UEMIT_SCOPE_FINALLY 3U

typedef struct UFuncState {
    struct UFuncState *parent;
    UProto   *proto;
    int       nactvar;            /* active locals, pins included */
    uint8_t   freereg;            /* first free register; == register of the next temporary */
    uint8_t   max_reg;            /* highest register this function has used */
    uint8_t   r_globals;          /* register holding the realm's globals object, loaded at entry */
    uint8_t   r_nargs;            /* register holding the passed-argument count; 0xFF when nparams == 0 */
    ULocal    locals[UFS_MAX_LOCALS];
    UUpval    upvals[UFS_MAX_UPVALUES]; int nupvals;
    UBlock    blocks[UFS_MAX_BLOCKS];   int nblocks;
    ULoop     loops[UEMIT_LOOP_MAX];    int nloops;
    UScope    scopes[UEMIT_SCOPE_MAX];  int nscopes;
    uint16_t  nsites;             /* next site index */
    const char **site_names;      /* interned names; grows by doubling through the module allocator */
    uint16_t  site_cap;
    uint32_t  prev_line;          /* 0 until this function's first instruction */
    uint32_t  ncaptures;          /* bumped each time a nested function captures one of these locals */
} UFuncState;

struct UEmitter {
    UProto      *module;
    UArena      *arena;           /* the per-statement arena the driver resets */
    UArena       fs_arena;        /* function states live here for the compile session */
    UFuncState  *fs_free;         /* closed function states, reused by the next ufunc_open */
    struct UVM  *vm;
    UFuncState  *fs;              /* current function */
    UEmitError   error;
    bool         any_stmt;
    uint8_t      last_result;     /* register of the last chunk-top statement's value */
    bool         lazy_arg_ctx;    /* compiling an argument to a lazy parameter: do not force */
    /* Chunk-top `var` bindings initialised with a function literal that
     * has a lazy parameter (ULOCAL_GLOBAL_DECL records).  They live here
     * rather than among the root's locals so that a declaration made
     * while a pin is live cannot land between the pin and its unpin. */
    ULocal       lazy_globals[UEMIT_LAZY_GLOBALS_MAX];
    int          nlazy_globals;
    UEmitDiag   *diag_buf; int diag_count, diag_cap;
};

/* --- register discipline (uemit_reg.c) --- */

uint8_t   ureg_alloc(UEmitter *e);                       /* freereg++, bumps max_reg, EMIT_REG_EXHAUSTED at 255 */
void      ureg_free_to(UEmitter *e, uint8_t r);          /* freereg = r; never below the locals' top */
uint8_t   ureg_top(const UEmitter *e);                   /* register above the last local or pin */
int       ulocal_declare(UEmitter *e, const char *name); /* a named local at freereg; returns its index */
uint8_t   upin(UEmitter *e);                             /* a nameless local at freereg; returns its register */
void      uunpin(UEmitter *e, uint8_t r);                /* pops the pin, which must be the newest local */
int       ulocal_find(const UFuncState *fs, const char *name);          /* innermost first; -1 */
int       uupval_find_or_install(UEmitter *e, UFuncState *fs, const char *name);  /* -1 when not an enclosing local */
bool      ublock_open(UEmitter *e, bool is_loop);
bool      ublock_close(UEmitter *e);                     /* emits CLOSE when a captured local dies; restores nactvar and freereg */
UFuncState *ufunc_open(UEmitter *e, UProto *proto);      /* parent = e->fs */
void      ufunc_close(UEmitter *e);                      /* writes max_reg/nupvals/sites into the proto, pops */

/* Instruction stream, constants, lines, sites. */
int       uinstr_emit(UEmitter *e, uint32_t ins, uint32_t line);   /* returns its pc */
int       uinstr_pc(const UEmitter *e);
void      uinstr_patch(UEmitter *e, int pc, uint32_t ins);
int       ujmp_emit(UEmitter *e, uint32_t line);                   /* forward JMP placeholder */
void      ujmp_patch_here(UEmitter *e, int jmp_pc);                /* Bx = BIAS + (here - jmp_pc - 1); EMIT_JUMP_TOO_FAR past int16 */
void      ujmp_patch_to(UEmitter *e, int jmp_pc, int target_pc);   /* same, to a known forward target */
void      ujmp_back(UEmitter *e, int target_pc, uint32_t line);    /* Bx = BIAS + (target - here) */
uint16_t  uconst_int(UEmitter *e, int64_t v);
uint16_t  uconst_float(UEmitter *e, double v);
uint16_t  uconst_str(UEmitter *e, const char *interned);
uint16_t  usite(UEmitter *e, const char *interned_name);           /* EMIT_TOO_MANY_SITES past 65535 */
/* A slot opcode with its site; emits EXTARG first when site > 255. */
void      uslot_emit(UEmitter *e, UOpcode op, uint8_t a, uint8_t b, uint16_t site, uint32_t line);

/* Latch `code` (first error wins).  Returns false so arms can write
 * `return uemit_fail(e, ...)` where they return a success flag. */
bool      uemit_fail(UEmitter *e, UEmitError code);
/* Intern `n` bytes; latches EMIT_OOM and returns NULL on failure. */
const char *uemit_intern(UEmitter *e, const char *bytes, int n);

/* --- expressions (uemit_expr.c) --- */

uint8_t uexpr_any(UEmitter *e, UAstNode *n);              /* value in some register; a local's own register when it is one */
void    uexpr_to(UEmitter *e, UAstNode *n, uint8_t dst);  /* materialise into dst */
uint8_t uexpr_next(UEmitter *e, UAstNode *n);             /* materialise into a fresh temporary at freereg; used for call frames */
/* Compile for effect: the value is discarded and every temporary the
 * statement used is released. */
void    ustmt(UEmitter *e, UAstNode *n);
/* A function literal into a fresh temporary at freereg.  `as_expression`
 * false makes the function return nil instead of its body's value. */
uint8_t uexpr_function(UEmitter *e, UAstNode **params, int nparams,
                       UAstNode *body, bool as_expression);
/* The lazy mask a FUNCTION literal's parameter list implies. */
uint16_t ufunc_lazy_mask(const UAstNode *fn);

/* --- control flow (uemit_ctrl.c) ---
 *
 * Each arm leaves its value in `want` when want >= 0, else in a register
 * it returns: a local's own register, or the topmost temporary. */

uint8_t uctrl_seq(UEmitter *e, UAstNode *n, int want);
uint8_t uctrl_block(UEmitter *e, UAstNode *n, int want);
uint8_t uctrl_if(UEmitter *e, UAstNode *n, int want);
uint8_t uctrl_while(UEmitter *e, UAstNode *n, int want);
uint8_t uctrl_switch(UEmitter *e, UAstNode *n, int want);
uint8_t uctrl_break(UEmitter *e, const UAstNode *n, int want);
uint8_t uctrl_continue(UEmitter *e, const UAstNode *n, int want);
uint8_t uctrl_return(UEmitter *e, UAstNode *n, int want);
uint8_t uctrl_throw(UEmitter *e, UAstNode *n, int want);
/* Kinds the reactive arms compile; refused here. */
uint8_t uctrl_unsupported(UEmitter *e, UAstNode *n);

/* --- try and tag scopes (uemit_scope.c) --- same `want` contract. */

uint8_t uscope_try(UEmitter *e, UAstNode *n, int want);   /* AST_TRY */
uint8_t uscope_tag(UEmitter *e, UAstNode *n, int want);   /* AST_TAG_PREFIX */
int     uscope_depth(const UEmitter *e);                  /* cleanup entries this frame has open */

/* `want` when it names a register, else a fresh temporary. */
static inline uint8_t uemit_target(UEmitter *e, int want) {
    return want >= 0 ? (uint8_t)want : ureg_alloc(e);
}

#endif /* UEMIT_INTERNAL_H */
