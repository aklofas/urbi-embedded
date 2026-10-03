/* SPDX-License-Identifier: BSD-3-Clause */
/* Temporary driver stub while the emitter is rewritten: every statement
 * is refused, so ufront_compile reports a compile error and the rest of
 * the tree keeps building. */
#include "emit/uemit.h"
void uemit_init(UEmitter *e, UProto *root, UArena *arena, struct UVM *vm, const char *source_name)
{ (void)arena; (void)vm; (void)source_name; e->module = root; e->error = EMIT_OK; e->diag_buf = NULL; e->diag_count = 0; e->diag_cap = 0; }
UEmitError uemit_statement(UEmitter *e, UAstNode *stmt) { (void)stmt; e->error = EMIT_UNSUPPORTED_AST; return e->error; }
/* The driver signature is the real emitter's, which writes through e. */
/* cppcheck-suppress constParameterPointer */
UEmitError uemit_finish(UEmitter *e) { return e->error; }
void urbi_emit_abandon(UEmitter *e) { (void)e; }
const char *uemit_error_name(UEmitError code) { (void)code; return "EMIT_UNSUPPORTED_AST"; }
bool urbi_emit_cond_has_direct_side_effect(UAstNode *n) { (void)n; return false; }
