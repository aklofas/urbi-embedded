/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit.c — the emitter's driver: create, one statement at a time,
 * finish or abandon.  The chunk top is compiled as the root function:
 * R0 holds the realm's globals, loaded by its first instruction, and a
 * chunk-top `var` declares a slot there rather than a local. */

#include "emit/uemit_internal.h"
#include "chunk/uchunk.h"
#include "parse/uast.h"
#include "util/uarena.h"

#include <stddef.h>
#include <stdint.h>

#if __STDC_HOSTED__
/* Deep-copy source_name into the root with the module's allocator. */
static void copy_source_name(UEmitter *e, const char *src) {
    if (src == NULL) return;
    size_t len = urbi_strlen(src);
    char *copy = (char *)emit_alloc_for(e->module)(NULL, len + 1U, e->module->alloc_ud);
    if (copy == NULL) { (void)uemit_fail(e, EMIT_OOM); return; }
    emit_memcpy(copy, src, len + 1U);
    e->module->source_name = copy;
}
#else
/* Freestanding builds compile on the host in every real use; the name
 * only feeds hosted diagnostics. */
static void copy_source_name(UEmitter *e, const char *src) {
    (void)e;
    (void)src;
}
#endif

UEmitter *uemit_new(UProto *root, UArena *arena, struct UVM *vm, const char *source_name) {
    if (arena->alloc_fn == NULL) return NULL;
    UEmitter *e = (UEmitter *)arena->alloc_fn(sizeof(UEmitter), arena->alloc_ud);
    if (e == NULL) return NULL;
    urbi_zero(e, sizeof *e);
    e->module = root;
    e->arena = arena;
    e->vm = vm;
    /* Function states outlive the per-statement arena the driver resets,
     * so they come from an arena of their own over the same allocator. */
    uarena_init_ex(&e->fs_arena, sizeof(UFuncState) + 64U,
                   arena->alloc_fn, arena->free_fn, arena->alloc_ud);
    if (vm != NULL) root->origin_vm = vm;
    root->arity_prologue = 1U;
    copy_source_name(e, source_name);
    UFuncState *fs = ufunc_open(e, root);
    if (fs != NULL) {
        fs->r_globals = upin(e);
        (void)uinstr_emit(e, uinstr_enc_abc(OP_LOAD_REALM_GLOBAL, fs->r_globals, 0U, 0U), 0U);
    }
    return e;
}

UEmitError uemit_statement(UEmitter *e, UAstNode *stmt) {
    if (e->error != EMIT_OK) return e->error;
    uint8_t r = uexpr_any(e, stmt);
    if (e->error != EMIT_OK) return e->error;
    e->last_result = r;
    e->any_stmt = true;
    ureg_free_to(e, ureg_top(e));
    return EMIT_OK;
}

static void set_root_recursive(UProto *node, UProto *root) {
    if (node == NULL) return;
    node->root = (node == root) ? NULL : root;
    for (size_t i = 0U; i < node->nested_count; i++) set_root_recursive(node->nested[i], root);
}

/* Frees everything the emitter owns, itself included.  Function states
 * still open (an error part-way through a function) hold a site-name
 * array that ufunc_close would otherwise have handed to their proto. */
static void emitter_free(UEmitter *e) {
    UChunkAllocFn alloc = emit_alloc_for(e->module);
    for (UFuncState *fs = e->fs; fs != NULL; fs = fs->parent) {
        if (fs->site_names != NULL) alloc((void *)fs->site_names, 0, e->module->alloc_ud);
        fs->site_names = NULL;
    }
    urbi_emit_diag_free_all(e);
    UFreeFn free_fn = e->fs_arena.free_fn;
    void *ud = e->fs_arena.alloc_ud;
    uarena_destroy(&e->fs_arena);
    if (free_fn != NULL) free_fn(e, ud);
}

UEmitError uemit_finish(UEmitter *e) {
    if (e->error == EMIT_OK) {
        uint8_t r = e->last_result;
        if (!e->any_stmt) {
            r = ureg_alloc(e);
            (void)uinstr_emit(e, uinstr_enc_abc(OP_LOADNIL, r, 0U, 0U), e->fs->prev_line);
        }
        (void)uinstr_emit(e, uinstr_enc_abc(OP_RET, r, 0U, 0U), e->fs->prev_line);
        ufunc_close(e);
    }
    UEmitError rc = e->error;
    if (rc == EMIT_OK) {
        set_root_recursive(e->module, e->module);
        e->module->total_proto_count = (uint16_t)(e->module->next_proto_serial + 1U);
    }
    emitter_free(e);
    return rc;
}

void urbi_emit_abandon(UEmitter *e) {
    if (e != NULL) emitter_free(e);
}

UEmitError uemit_error(const UEmitter *e) { return e->error; }
int uemit_diag_count(const UEmitter *e) { return e->diag_count; }
const UEmitDiag *uemit_diag_at(const UEmitter *e, int i) {
    return (i >= 0 && i < e->diag_count) ? &e->diag_buf[i] : NULL;
}

const char *uemit_error_name(UEmitError code) {
    switch (code) {
    case EMIT_OK:                     return "EMIT_OK";
    case EMIT_OOM:                    return "EMIT_OOM";
    case EMIT_AST_ERROR:              return "EMIT_AST_ERROR";
    case EMIT_UNSUPPORTED_AST:        return "EMIT_UNSUPPORTED_AST";
    case EMIT_REG_EXHAUSTED:          return "EMIT_REG_EXHAUSTED";
    case EMIT_CONSTANT_POOL_FULL:     return "EMIT_CONSTANT_POOL_FULL";
    case EMIT_FINISHED:               return "EMIT_FINISHED";
    case EMIT_UPVAL_EXHAUSTED:        return "EMIT_UPVAL_EXHAUSTED";
    case EMIT_LOCAL_REDECLARE:        return "EMIT_LOCAL_REDECLARE";
    case EMIT_NESTING_TOO_DEEP:       return "EMIT_NESTING_TOO_DEEP";
    case EMIT_LAZY_ON_METHOD:         return "EMIT_LAZY_ON_METHOD";
    case EMIT_LAZY_PARAM_ASSIGN:      return "EMIT_LAZY_PARAM_ASSIGN";
    case EMIT_TOO_MANY_SITES:         return "EMIT_TOO_MANY_SITES";
    case EMIT_NO_THIS_OUTSIDE_METHOD: return "EMIT_NO_THIS_OUTSIDE_METHOD";
    case EMIT_TOO_MANY_ARGS:          return "EMIT_TOO_MANY_ARGS";
    case EMIT_PATCH_LIST_FULL:        return "EMIT_PATCH_LIST_FULL";
    case EMIT_JUMP_TOO_FAR:           return "EMIT_JUMP_TOO_FAR";
    }
    return "EMIT_UNKNOWN";
}

/* Best-effort compile-time check: true when `n` contains a direct write
 * (assignment, declaration, member set).  Used to warn when a watcher
 * condition mutates state.  A call is opaque (false) so read-only
 * methods do not warn, and a function literal's body runs elsewhere. */
bool urbi_emit_cond_has_direct_side_effect(UAstNode *n) {
    if (n == NULL) return false;
    switch (n->kind) {
    case AST_ASSIGN:
    case AST_VAR_DECL:
    case AST_MEMBER_SET:
        return true;
    case AST_SEQ:
        for (int i = 0; i < n->u.seq.count; i++)
            if (urbi_emit_cond_has_direct_side_effect(n->u.seq.children[i])) return true;
        return false;
    case AST_BLOCK:
        for (int i = 0; i < n->u.block.count; i++)
            if (urbi_emit_cond_has_direct_side_effect(n->u.block.stmts[i])) return true;
        return false;
    case AST_BINARY:
        return urbi_emit_cond_has_direct_side_effect(n->u.binary.lhs)
            || urbi_emit_cond_has_direct_side_effect(n->u.binary.rhs);
    case AST_UNARY:
        return urbi_emit_cond_has_direct_side_effect(n->u.unary.operand);
    case AST_COMPARE:
        return urbi_emit_cond_has_direct_side_effect(n->u.cmp.lhs)
            || urbi_emit_cond_has_direct_side_effect(n->u.cmp.rhs);
    case AST_LOGICAL:
        return urbi_emit_cond_has_direct_side_effect(n->u.logical.lhs)
            || urbi_emit_cond_has_direct_side_effect(n->u.logical.rhs);
    case AST_INT: case AST_IDENT: case AST_ERROR: case AST_BOOL: case AST_NIL:
    case AST_NOOP: case AST_IF: case AST_WHILE: case AST_FUNCTION: case AST_CALL:
    case AST_RETURN: case AST_PARAM: case AST_TRY: case AST_THROW: case AST_TAG_PREFIX:
    case AST_MEMBER_GET: case AST_WATCHER: case AST_STR: case AST_FLOAT_LIT: case AST_THIS:
    case AST_BREAK: case AST_CONTINUE: case AST_SWITCH:
        return false;
    }
    return false;
}
