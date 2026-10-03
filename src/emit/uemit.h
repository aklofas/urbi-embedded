/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode emitter driver interface, diagnostics, disassembler and chunk
 * writer.  AST -> root UProto.
 *
 * The emitter is opaque outside src/emit: a driver creates one with
 * uemit_new, feeds it statements, and either finishes it (which frees
 * it) or abandons it on an error path.  Diagnostics are read through
 * uemit_diag_count / uemit_diag_at while the emitter is alive. */

#ifndef UEMIT_H
#define UEMIT_H

#include <stdarg.h>               /* va_list — urbi_emit_diag_warn variadic */
#include <stdbool.h>
#include <stddef.h>               /* ptrdiff_t */
#include <stdint.h>

#include "util/uarena.h"
#include "parse/uast.h"
#include "chunk/uchunk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- emit-time diagnostic (warn/error) plumbing --- */

typedef struct {
    enum { UEMIT_DIAG_WARN = 0, UEMIT_DIAG_ERROR = 1 } level;
    int         line;
    int         col;
    const char *message;    /* allocator-owned copy; freed by urbi_emit_diag_free_all */
} UEmitDiag;

/* --- emit-time errors (distinct from loader errors) --- */

typedef enum {
    EMIT_OK = 0,
    EMIT_OOM,                     /* a buffer grow failed */
    EMIT_AST_ERROR,               /* input AST contained AST_ERROR */
    EMIT_UNSUPPORTED_AST,         /* AST kind not emittable */
    EMIT_REG_EXHAUSTED,           /* a function needs more than 255 registers or 200 locals */
    EMIT_CONSTANT_POOL_FULL,      /* > 65535 constants — Bx overflow */
    EMIT_FINISHED,                /* uemit_statement called after uemit_finish */
    EMIT_UPVAL_EXHAUSTED,         /* too many captures */
    EMIT_LOCAL_REDECLARE,         /* duplicate `var x` in one scope */
    EMIT_NESTING_TOO_DEEP,        /* block or loop nesting cap exceeded */
    EMIT_LAZY_ON_METHOD,          /* a function with a lazy parameter stored as a method */
    EMIT_LAZY_PARAM_ASSIGN,       /* assignment to a lazy parameter */
    EMIT_TOO_MANY_SITES,          /* a function needs more than 65,535 slot sites */
    EMIT_NO_THIS_OUTSIDE_METHOD,  /* `this` used at chunk top */
    EMIT_TOO_MANY_ARGS,           /* call with more than 252 arguments */
    EMIT_PATCH_LIST_FULL,         /* too many break/continue sites in one loop */
    EMIT_JUMP_TOO_FAR             /* a jump offset does not fit 16 signed bits */
} UEmitError;

typedef struct UEmitter UEmitter;

/* --- driver API --- */

/* A fresh emitter compiling into `root`, allocated through the arena's
 * allocator.  `arena` is the per-statement AST arena the driver resets;
 * the emitter only borrows it.  Copies `source_name` into the root.
 * Returns NULL when the allocation fails. */
UEmitter *uemit_new(UProto *root, UArena *arena, struct UVM *vm, const char *source_name);

/* Emit one statement's bytecode.  The first error latches; later calls
   return it without touching the chunk. */
UEmitError uemit_statement(UEmitter *e, UAstNode *stmt);

/* Return the last statement's value (nil when there was none), close the
   root function and free the emitter.  Returns the first error, or
   EMIT_OK.  `e` is invalid afterwards. */
UEmitError uemit_finish(UEmitter *e);

/* Error-path teardown: free the emitter and everything it owns.  The
   root and the protos already hanging from it are left for
   uchunk_destroy.  `e` is invalid afterwards; NULL is a no-op. */
void urbi_emit_abandon(UEmitter *e);

/* Debug helper. */
const char *uemit_error_name(UEmitError code);

/* The latched error, or EMIT_OK. */
UEmitError uemit_error(const UEmitter *e);

/* The recorded diagnostics, in order. */
int uemit_diag_count(const UEmitter *e);
const UEmitDiag *uemit_diag_at(const UEmitter *e, int i);

/* Best-effort compile-time walker: returns true when n contains a direct
 * write (assignment, declaration, member set).  Calls are opaque. */
bool urbi_emit_cond_has_direct_side_effect(UAstNode *n);

/* --- diagnostics (uemit_diag.c) --- */

/* Append a warn-level diagnostic to the emitter's diag buffer.
 * n may be NULL (position will be 0,0).  fmt is a printf-style format
 * string.  Does not set the error; emit continues normally.
 * If the buffer cannot grow (OOM), the diagnostic is silently dropped. */
void urbi_emit_diag_warn(UEmitter *e, const UAstNode *n, const char *fmt, ...);

/* Append an error-level diagnostic to the emitter's diag buffer.
 * Callers MUST still latch the error — this only enriches the record with
 * source position and a human message. */
void urbi_emit_diag_error(UEmitter *e, const UAstNode *n, const char *fmt, ...);

/* Format the first ERROR-level diagnostic as "<source>:<line>:<col>: <msg>"
 * (or "<source>: <msg>" when line is 0) into buf[0..cap-1].  When the module
 * carries no source name, "<stdin>" is used.  Returns false when no error
 * diagnostic has been recorded (buf is untouched).
 * No-op / returns false on freestanding builds. */
bool urbi_emit_diag_format_first_error(const UEmitter *e, char *buf, size_t cap);

/* Free all diagnostic message strings and the diag buffer itself.
 * No-op on freestanding builds. */
void urbi_emit_diag_free_all(UEmitter *e);

/* --- disassembler and writer --- */

/* Write a human-readable disassembly of the chunk rooted at `root` into
   buf: the root proto's instructions first, then every nested proto in
   depth-first order under a "; proto P<path>" header, each followed by
   its constant pool.  <path> is the proto's index in its parent's nested
   list, prefixed by its parent's path and a dot below the root (P0,
   P0.1, ...), so it names the index a CLOSURE in the parent prints.
   Returns bytes written (excluding the terminator).  Truncates if cap is
   too small; always null-terminates when cap > 0. */
size_t uemit_disassemble(const UProto *root, char *buf, size_t cap);

/* Completeness check: returns 1 if every opcode in [0, OP_MAX) has a
 * name.  Used by the disassembler unit test. */
int urbi_emit_disasm_opnames_complete(void);

/* Serialize a root UProto to the .urb byte format.
   With buf == NULL, returns the number of bytes the chunk needs.
   Otherwise returns bytes written, or a negative value on failure:
   -(ptrdiff_t)UChunkLoadError code (UCHUNK_LOAD_TRUNCATED when cap is
   too small). */
ptrdiff_t uchunk_serialize(const UProto *root, uint8_t *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
