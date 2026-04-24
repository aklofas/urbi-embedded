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
    EMIT_FINISHED                 /* uemit_statement called after uemit_finish */
} UEmitError;

/* --- UEmitter state (caller stack-allocates, emitter fills) --- */

typedef struct UEmitter {
    UModule       *module;           /* non-owning; caller supplies */
    UArena       *arena;           /* non-owning; currently unused at M1 but reserved */
    uint8_t      next_reg;        /* register allocator cursor */
    uint8_t      max_reg_seen;    /* highest slot ever used */
    uint8_t      last_result_reg; /* register of most recent statement's result */
    uint32_t     prev_line;       /* last emitted instruction's source line */
    bool         any_stmt_emitted;/* gates OP_RET at finish */
    bool         finished;
    UEmitError    error;           /* sticky: first error latches */
} UEmitter;

/* --- API --- */

/* Initialize.  module and arena must both outlive the emitter.
   source_name may be NULL. */
void uemit_init(UEmitter *e, UModule *module, UArena *arena, const char *source_name);

/* Emit one statement's bytecode into the module.  stmt must be non-NULL.
   AST_ERROR nodes are rejected with EMIT_AST_ERROR.  On first error, the
   error latches; subsequent calls return it without touching the module. */
UEmitError uemit_statement(UEmitter *e, UAstNode *stmt);

/* Finalize: emit OP_RET (if any statement was emitted) and record max_reg.
   Further uemit_statement calls return EMIT_FINISHED.  Returns the first
   accumulated error, or EMIT_OK. */
UEmitError uemit_finish(UEmitter *e);

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
