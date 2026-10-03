/* SPDX-License-Identifier: BSD-3-Clause */
/* src/emit/ufront.c — see ufront.h. */

#include "emit/ufront.h"

#include <stdio.h>
#include <string.h>

#include "rt/uexec.h"
#include "lex/ulex.h"
#include "parse/uparse.h"
#include "parse/uast.h"
#include "emit/uemit.h"
#include "util/uarena.h"
#include "chunk/uchunk.h"

/* --- the intern seam (see the header banner) -------------------------- */

const char *ustr_intern(struct UVM *vm, const char *bytes, size_t nbytes)
{
    if (vm == NULL || bytes == NULL) return NULL;
    USym *s = usym_intern(vm, bytes, nbytes);
    return s ? s->bytes : NULL;
}

/* --- compile ----------------------------------------------------------- */

static void *ufront_chunk_alloc(void *ptr, size_t nbytes, void *ud)
{
    UVM *vm = (UVM *)ud;
    return vm->gc.alloc(ptr, nbytes, vm->gc.alloc_ud);
}

int ufront_compile(struct UVM *vm, const char *src, size_t n, const char *name,
                   const UCompileBudget *budget,
                   struct UProto **out, char *err, size_t errcap)
{
    if (!vm || !src || !out) return URBI_ERR_INVALID_ARG;
    *out = NULL;
    if (errcap > 0 && err) err[0] = '\0';

    /* The source-bytes limit is the one that is cheaper to check than to
     * hit: refusing here means never lexing the text at all. */
    if (budget && budget->max_source_bytes > 0 && n > budget->max_source_bytes) {
        if (err && errcap) snprintf(err, errcap, "compile-budget exceeded: source");
        return URBI_ERR_COMPILE_BUDGET_SOURCE;
    }

    ULexer lex;
    ulex_init(&lex, src, n);

    UArena arena;
    uarena_init(&arena, 4096);

    UProto *root = (UProto *)ufront_chunk_alloc(NULL, sizeof(UProto), vm);
    if (root == NULL) {
        if (err && errcap) snprintf(err, errcap, "out of memory");
        uarena_destroy(&arena);
        return URBI_ERR_OOM;
    }
    memset(root, 0, sizeof *root);
    root->alloc_fn       = ufront_chunk_alloc;
    root->alloc_ud       = vm;
    root->heap_allocated = true;

    UEmitter *e = uemit_new(root, &arena, vm, name);
    if (e == NULL) {
        if (err && errcap) snprintf(err, errcap, "out of memory");
        uchunk_destroy(root, NULL);
        uarena_destroy(&arena);
        return URBI_ERR_OOM;
    }

    UParser p;
    uparse_init(&p, &lex, &arena);
    uparse_set_budget(&p, budget);

    bool has_error = false;
    const char *parse_errmsg = NULL;
    int parse_err_line = 0, parse_err_col = 0;
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) {
            parse_errmsg   = node->u.err.message;
            parse_err_line = node->line;
            parse_err_col  = node->col;
            has_error = true;
            break;
        }
        if (uemit_statement(e, node) != EMIT_OK) { has_error = true; break; }
        uarena_reset(&arena);
    }

    /* Warnings go to the diag channel the same way the old REPL printed
     * them.  They are read before uemit_finish, which frees the emitter
     * and its diagnostics with it. */
    if (!has_error) {
        const char *warn_src = uproto_source_name(root);
        if (warn_src == NULL) warn_src = "<stdin>";
        for (int di = 0; di < uemit_diag_count(e); di++) {
            const UEmitDiag *d = uemit_diag_at(e, di);
            if (d->level == UEMIT_DIAG_WARN)
                fprintf(stderr, "%s:%d:%d: warning: %s\n", warn_src, d->line, d->col, d->message);
        }
    }

    UEmitError emit_rc = EMIT_OK;
    if (!has_error) {
        emit_rc = uemit_finish(e);
        e = NULL;
        if (emit_rc != EMIT_OK) has_error = true;
    } else {
        emit_rc = uemit_error(e);
    }

    if (has_error) {
        /* A crossed limit stops the parser by starving its node
         * allocator, which surfaces as an ordinary parse error; asking
         * the parser which limit it was is what tells the two apart. */
        int budget_err = uparse_budget_err(&p);
        if (err && errcap) {
            if (budget_err != URBI_OK) {
                snprintf(err, errcap, "compile-budget exceeded: %s",
                         budget_err == URBI_ERR_COMPILE_BUDGET_DEPTH ? "depth" : "nodes");
            } else if (parse_errmsg && (parse_err_line > 0 || parse_err_col > 0)) {
                snprintf(err, errcap, "%s:%d:%d: %s", ulex_current_source(&lex),
                         parse_err_line, parse_err_col, parse_errmsg);
            } else if (e == NULL || !urbi_emit_diag_format_first_error(e, err, errcap)) {
                snprintf(err, errcap, "%s: %s",
                         name ? name : "<stdin>",
                         parse_errmsg ? parse_errmsg : uemit_error_name(emit_rc));
            }
        }
        urbi_emit_abandon(e);
        uchunk_destroy(root, NULL);   /* heap_allocated: frees the struct too */
        uarena_destroy(&arena);
        if (budget_err != URBI_OK) return budget_err;
        return emit_rc == EMIT_OOM ? URBI_ERR_OOM : URBI_ERR_COMPILE;
    }

    uarena_destroy(&arena);
    *out = root;
    return URBI_OK;
}

void ufront_disassemble(const struct UProto *root, const char *name)
{
    char buf[16384];
    size_t n = uemit_disassemble(root, buf, sizeof buf);
    if (name) printf("; %s\n", name);
    fwrite(buf, 1, n, stdout);
    if (n > 0 && buf[n - 1] != '\n') fputc('\n', stdout);
}

ptrdiff_t ufront_serialize(const struct UProto *root, unsigned char *buf, size_t cap)
{
    return uchunk_serialize(root, (uint8_t *)buf, cap);
}
