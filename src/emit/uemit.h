/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode emitter driver interface, diagnostics, disassembler and chunk
 * writer.  AST -> root UProto.  Hosted.
 *
 * The emitter itself is being rewritten against wire v2; until it lands,
 * src/emit/uemit_stub.c provides the driver entry points and refuses
 * every statement.  UEmitter below carries only what the driver, the
 * diagnostic buffer (uemit_diag.c) and the stub need. */

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
    EMIT_OOM,                     /* module buffer grow failed */
    EMIT_AST_ERROR,               /* input AST contained AST_ERROR */
    EMIT_UNSUPPORTED_AST,         /* AST kind not emittable */
    EMIT_REG_EXHAUSTED,           /* > 255 registers needed — deep expression */
    EMIT_CONSTANT_POOL_FULL,      /* > 65535 constants — Bx overflow */
    EMIT_LINE_OVERFLOW,           /* source line > UINT32_MAX (effectively unreachable) */
    EMIT_FINISHED,                /* uemit_statement called after uemit_finish */
    EMIT_UPVAL_EXHAUSTED,         /* too many captures */
    EMIT_LOCAL_REDECLARE,         /* duplicate `var x` in same block */
    EMIT_UNRESOLVED_NAME,         /* identifier not local/upvalue/global */
    EMIT_NESTING_TOO_DEEP,        /* block or function nesting cap exceeded */
    EMIT_BARE_LAZY_FUNCTION,      /* `function name { body }` */
    EMIT_CLOSURE_KEYWORD,         /* `closure(x){...}` */
    EMIT_LAZY_ON_METHOD,          /* Lazy on method-bound function */
    EMIT_LAZY_PARAM_ASSIGN,       /* Assignment to lazy param */
    EMIT_TOO_MANY_SITES,          /* a function needs more than 65,535 slot sites */
    EMIT_RESERVED_KEYWORD_AS_IDENT, /* `var at = 1` — hard keyword as variable name */
    EMIT_TOO_MANY_ARGS,           /* call with >= 254 args (B encodes nargs+1) */
    EMIT_NO_THIS_OUTSIDE_METHOD,  /* `this` used at top level */
    EMIT_PATCH_LIST_FULL          /* too many break/continue sites in one loop */
} UEmitError;

/* --- UEmitter state (caller stack-allocates, emitter fills) --- */

typedef struct UEmitter {
    UProto      *module;          /* non-owning root UProto; caller supplies */
    UEmitError   error;           /* sticky: first error latches */
    /* Diagnostic buffer.  urbi_emit_diag_warn / _error append here;
     * diag_buf is module-allocator-owned and grows by doubling. */
    UEmitDiag   *diag_buf;
    int          diag_count;
    int          diag_cap;
} UEmitter;

/* --- driver API --- */

void uemit_init(UEmitter *e, UProto *root, UArena *arena,
                struct UVM *vm, const char *source_name);

/* Emit one statement's bytecode into the module.  On first error, the
   error latches; subsequent calls return it without touching the module. */
UEmitError uemit_statement(UEmitter *e, UAstNode *stmt);

/* Finalize the module.  Returns the first accumulated error, or EMIT_OK. */
UEmitError uemit_finish(UEmitter *e);

/* Driver error-path teardown: free emitter-owned storage without
   finishing the module.  Idempotent, and safe after uemit_finish. */
void urbi_emit_abandon(UEmitter *e);

/* Debug helper. */
const char *uemit_error_name(UEmitError code);

/* Best-effort compile-time walker: returns true when n contains a direct
 * write (assignment, declaration, member set).  Calls are opaque. */
bool urbi_emit_cond_has_direct_side_effect(UAstNode *n);

/* --- diagnostics (uemit_diag.c) --- */

/* Append a warn-level diagnostic to the emitter's diag buffer.
 * n may be NULL (position will be 0,0).  fmt is a printf-style format
 * string.  Does not set e->error; emit continues normally.
 * If the buffer cannot grow (OOM), the diagnostic is silently dropped. */
void urbi_emit_diag_warn(UEmitter *e, const UAstNode *n, const char *fmt, ...);

/* Append an error-level diagnostic to the emitter's diag buffer.
 * Callers MUST still set e->error — this only enriches the record with
 * source position and a human message; it does not latch the error. */
void urbi_emit_diag_error(UEmitter *e, const UAstNode *n, const char *fmt, ...);

/* Format the first ERROR-level diagnostic as "<source>:<line>:<col>: <msg>"
 * (or "<source>: <msg>" when line is 0) into buf[0..cap-1].  When the module
 * carries no source name, "<stdin>" is used.  Returns false when no error
 * diagnostic has been recorded (buf is untouched).
 * No-op / returns false on freestanding builds. */
bool urbi_emit_diag_format_first_error(const UEmitter *e, char *buf, size_t cap);

/* Free all diagnostic message strings and the diag_buf array itself.
 * Resets diag_count/diag_cap to 0.  No-op on freestanding builds. */
void urbi_emit_diag_free_all(UEmitter *e);

/* --- disassembler and writer --- */

/* Write a human-readable disassembly of the chunk rooted at `root` into
   buf: the root proto's instructions first, then every nested proto in
   depth-first order under a "; proto P<n>" header, each followed by its
   constant pool.  Returns bytes written (excluding the terminator).
   Truncates if cap is too small; always null-terminates when cap > 0. */
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
