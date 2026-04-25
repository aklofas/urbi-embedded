/* SPDX-License-Identifier: BSD-3-Clause */
/* Single-pass bytecode emitter.  AST -> UModule.  Hosted. */

#ifndef UEMIT_H
#define UEMIT_H

#include <stdbool.h>
#include <stddef.h>               /* ptrdiff_t */
#include <stdint.h>

#include "uarena.h"
#include "uast.h"
#include "umodule.h"

#ifdef __cplusplus
extern "C" {
#endif

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
    EMIT_LAZY_PARAM_ASSIGN        /* T16: assignment to lazy param */
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
    UEmitError    error;           /* sticky: first error latches */
    struct UFuncState *current_fs; /* M2: current compilation function */
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

/* Debug helper. */
const char *uemit_error_name(UEmitError code);

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

#ifdef __cplusplus
}
#endif

#endif
