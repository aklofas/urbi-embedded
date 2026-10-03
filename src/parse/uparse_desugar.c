/* SPDX-License-Identifier: BSD-3-Clause */
/* uparse_desugar.c — node builders the sugar forms lower through.
 * Every sugar form in the grammar ends here as a CALL, MEMBER_SET, BLOCK,
 * IF or THROW over the core kinds; the emitter never learns the sugar
 * existed. */
#include "parse/uparse_internal.h"
#include "util/uarena.h"

UAstNode *urbi_parse_desugar_ident(UParser *p, const char *name, int len, int line, int col) {
    return urbi_parse_make_ident(p, name, len, line, col);
}

UAstNode *urbi_parse_desugar_str(UParser *p, const char *bytes, int len, int line, int col) {
    UAstNode *n = urbi_parse_make_node(p, AST_STR, line, col);
    if (!n) return NULL;
    n->u.str_lit.bytes = bytes;
    n->u.str_lit.len   = len;
    return n;
}

UAstNode *urbi_parse_desugar_member_get(UParser *p, UAstNode *recv, const char *name, int len, int line, int col) {
    UAstNode *n = urbi_parse_make_node(p, AST_MEMBER_GET, line, col);
    if (!n) return NULL;
    n->u.member.recv = recv; n->u.member.name_start = name; n->u.member.name_len = len; n->u.member.value = NULL;
    return n;
}

UAstNode *urbi_parse_desugar_member_set(UParser *p, UAstNode *recv, const char *name, int len, UAstNode *value, int line, int col) {
    UAstNode *n = urbi_parse_desugar_member_get(p, recv, name, len, line, col);
    if (!n) return NULL;
    n->kind = AST_MEMBER_SET; n->u.member.value = value;
    return n;
}

UAstNode *urbi_parse_desugar_call(UParser *p, UAstNode *callee, UAstNode **args, int argc, int line, int col) {
    UAstNode *n = urbi_parse_make_node(p, AST_CALL, line, col);
    if (!n) return NULL;
    n->u.call.callee = callee; n->u.call.args = args; n->u.call.arg_count = argc;
    return n;
}

UAstNode *urbi_parse_desugar_var_decl(UParser *p, const char *name, int len, UAstNode *init, int line, int col) {
    UAstNode *n = urbi_parse_make_node(p, AST_VAR_DECL, line, col);
    if (!n) return NULL;
    n->u.var_decl.name_start = name; n->u.var_decl.name_len = len; n->u.var_decl.init = init;
    return n;
}

UAstNode *urbi_parse_desugar_block(UParser *p, UAstNode **stmts, int count, int line, int col) {
    UAstNode *n = urbi_parse_make_node(p, AST_BLOCK, line, col);
    if (!n) return NULL;
    n->u.block.stmts = stmts; n->u.block.count = count;
    return n;
}

/* "\x01" + tag + decimal serial.  The serial is per parser so two desugars
 * in one scope never collide; the leading byte keeps user code out. */
const char *urbi_parse_hidden_name(UParser *p, const char *tag, int *out_len) {
    char tmp[24]; int n = 0;
    tmp[n++] = UPARSE_HIDDEN_PREFIX;
    while (*tag && n < 12) tmp[n++] = *tag++;
    unsigned serial = p->hidden_serial++;
    char dig[10]; int nd = 0;
    do { dig[nd++] = (char)('0' + serial % 10u); serial /= 10u; } while (serial);
    while (nd) tmp[n++] = dig[--nd];
    char *out = (char *)uarena_alloc(p->arena, (size_t)n);
    if (!out) return NULL;
    for (int i = 0; i < n; i++) out[i] = tmp[i];
    *out_len = n;
    return out;
}
