/* SPDX-License-Identifier: BSD-3-Clause */
/* uemit_expr.c — expressions, calls and function literals, and the one
 * dispatcher every AST kind goes through.
 *
 * Every arm is written against `want`: a register the value must land
 * in, or -1 for "anywhere".  With -1 an arm returns either a local's own
 * register or the topmost temporary; the dispatcher then releases every
 * temporary above the value.  With a register it releases everything the
 * arm allocated.  Locals the arm declared stay: ureg_free_to never goes
 * below them. */

#include "emit/uemit_internal.h"
#include "emit/uintern.h"
#include "chunk/uchunk.h"
#include "parse/uast.h"

#include <stddef.h>
#include <stdint.h>

static uint8_t uexpr_want(UEmitter *e, UAstNode *n, int want);

static uint32_t line_of(const UAstNode *n) {
    return n->line > 0 ? (uint32_t)n->line : 0U;
}

/* The value is in `r`; move it to `want` when that names another
 * register.  Returns where the value is. */
static uint8_t settle(UEmitter *e, uint8_t r, int want, uint32_t line) {
    if (want < 0 || (uint8_t)want == r) return r;
    (void)uinstr_emit(e, uinstr_enc_abc(OP_MOVE, (uint8_t)want, r, 0U), line);
    return (uint8_t)want;
}

/* The register a computed value goes to: `want`, else the topmost
 * temporary among `a` and `b` (the operands just computed, which the
 * instruction reads before it writes), else a fresh one. */
static uint8_t result_reg(UEmitter *e, int want, uint8_t a, uint8_t b) {
    if (want >= 0) return (uint8_t)want;
    uint8_t top = ureg_top(e);
    if (a >= top) return a;
    if (b >= top) return b;
    return ureg_alloc(e);
}

static bool name_is(const char *s, int n, const char *lit) {
    int i = 0;
    for (; i < n && lit[i] != '\0'; i++) {
        if (s[i] != lit[i]) return false;
    }
    return i == n && lit[i] == '\0';
}

/* --- literals ------------------------------------------------------------- */

static uint8_t ex_loadk(UEmitter *e, uint16_t k, int want, uint32_t line) {
    uint8_t d = uemit_target(e, want);
    (void)uinstr_emit(e, uinstr_enc_abx(OP_LOADK, d, k), line);
    return d;
}

static uint8_t ex_simple(UEmitter *e, UOpcode op, uint8_t b, int want, uint32_t line) {
    uint8_t d = uemit_target(e, want);
    (void)uinstr_emit(e, uinstr_enc_abc(op, d, b, 0U), line);
    return d;
}

static uint8_t ex_this(UEmitter *e, const UAstNode *n, int want) {
    if (e->fs->parent == NULL) {
        urbi_emit_diag_error(e, n, "this used outside a method or nested closure");
        (void)uemit_fail(e, EMIT_NO_THIS_OUTSIDE_METHOD);
        return 0U;
    }
    return ex_simple(e, OP_LOAD_RECV, 0U, want, line_of(n));
}

/* --- names ---------------------------------------------------------------- */

static uint8_t ex_ident(UEmitter *e, const UAstNode *n, int want) {
    const char *name = uemit_intern(e, n->u.ident.start, n->u.ident.len);
    if (name == NULL) return 0U;
    UFuncState *fs = e->fs;
    uint32_t line = line_of(n);
    int li = ulocal_find(fs, name);
    if (li >= 0) {
        uint8_t r = fs->locals[li].reg;
        if ((fs->locals[li].flags & ULOCAL_LAZY_PARAM) != 0U && !e->lazy_arg_ctx) {
            /* A lazy parameter holds a thunk; reading it runs the thunk. */
            uint8_t t = ureg_alloc(e);
            (void)uinstr_emit(e, uinstr_enc_abc(OP_MOVE, t, r, 0U), line);
            (void)uinstr_emit(e, uinstr_enc_abc(OP_CALL, t, 1U, 2U), line);
            return settle(e, t, want, line);
        }
        return settle(e, r, want, line);
    }
    int ui = uupval_find_or_install(e, fs, name);
    if (e->error != EMIT_OK) return 0U;
    uint8_t d = uemit_target(e, want);
    if (ui >= 0) {
        (void)uinstr_emit(e, uinstr_enc_abc(OP_GETUPVAL, d, (uint8_t)ui, 0U), line);
    } else {
        uslot_emit(e, OP_GETSLOT, d, fs->r_globals, usite(e, name), line);
    }
    return d;
}

/* Records (or updates) the lazy mask of a chunk-top binding. */
static void lazy_global_note(UEmitter *e, const char *name, uint16_t mask) {
    for (int i = 0; i < e->nlazy_globals; i++) {
        if (e->lazy_globals[i].name == name) { e->lazy_globals[i].lazy_mask = mask; return; }
    }
    if (mask == 0U) return;
    if (e->nlazy_globals >= UEMIT_LAZY_GLOBALS_MAX) {
        urbi_emit_diag_error(e, NULL, "too many chunk-top functions with lazy parameters (max %d)",
                             UEMIT_LAZY_GLOBALS_MAX);
        (void)uemit_fail(e, EMIT_REG_EXHAUSTED);
        return;
    }
    ULocal *g = &e->lazy_globals[e->nlazy_globals++];
    g->name = name;
    g->reg = 0U;
    g->flags = ULOCAL_GLOBAL_DECL;
    g->lazy_mask = mask;
}

static uint16_t init_mask(const UAstNode *init) {
    return (init != NULL && init->kind == AST_FUNCTION) ? ufunc_lazy_mask(init) : 0U;
}

static uint8_t ex_var_decl(UEmitter *e, const UAstNode *n, int want) {
    const char *name = uemit_intern(e, n->u.var_decl.name_start, n->u.var_decl.name_len);
    if (name == NULL) return 0U;
    UFuncState *fs = e->fs;
    UAstNode *init = n->u.var_decl.init;
    uint32_t line = line_of(n);

    if (fs->parent == NULL && fs->nblocks == 0) {
        /* Chunk top: the binding is a slot of the realm's globals. */
        uint8_t v;
        if (init != NULL) {
            v = uexpr_next(e, init);
        } else {
            v = ureg_alloc(e);
            (void)uinstr_emit(e, uinstr_enc_abc(OP_LOADNIL, v, 0U, 0U), line);
        }
        uslot_emit(e, OP_SETSLOT, v, fs->r_globals, usite(e, name), line);
        lazy_global_note(e, name, init_mask(init));
        return settle(e, v, want, line);
    }

    int from = (fs->nblocks > 0) ? fs->blocks[fs->nblocks - 1].nactvar_on_enter : 0;
    for (int i = from; i < fs->nactvar; i++) {
        if (fs->locals[i].name == name) {
            urbi_emit_diag_error(e, n, "variable '%.*s' already declared in this scope",
                                 n->u.var_decl.name_len, n->u.var_decl.name_start);
            (void)uemit_fail(e, EMIT_LOCAL_REDECLARE);
            return 0U;
        }
    }
    /* The initializer is compiled before the name is visible, so it sees
     * any outer binding of the same name. */
    uint8_t t;
    if (init != NULL) {
        t = uexpr_next(e, init);
    } else {
        t = ureg_alloc(e);
        (void)uinstr_emit(e, uinstr_enc_abc(OP_LOADNIL, t, 0U, 0U), line);
    }
    ureg_free_to(e, t);
    int li = ulocal_declare(e, name);
    if (li < 0) return 0U;
    uint8_t r = fs->locals[li].reg;
    if (r != t) (void)uinstr_emit(e, uinstr_enc_abc(OP_MOVE, r, t, 0U), line);
    fs->locals[li].lazy_mask = init_mask(init);
    return settle(e, r, want, line);
}

static uint8_t ex_assign(UEmitter *e, const UAstNode *n, int want) {
    const char *name = uemit_intern(e, n->u.assign.name_start, n->u.assign.name_len);
    if (name == NULL) return 0U;
    UFuncState *fs = e->fs;
    UAstNode *value = n->u.assign.value;
    uint32_t line = line_of(n);
    int li = ulocal_find(fs, name);
    if (li >= 0) {
        if ((fs->locals[li].flags & ULOCAL_LAZY_PARAM) != 0U) {
            urbi_emit_diag_error(e, n, "cannot assign to lazy parameter '%.*s'",
                                 n->u.assign.name_len, n->u.assign.name_start);
            (void)uemit_fail(e, EMIT_LAZY_PARAM_ASSIGN);
            return 0U;
        }
        uint8_t r = fs->locals[li].reg;
        uexpr_to(e, value, r);
        fs->locals[li].lazy_mask = init_mask(value);
        return settle(e, r, want, line);
    }
    int ui = uupval_find_or_install(e, fs, name);
    if (e->error != EMIT_OK) return 0U;
    uint8_t v = uexpr_any(e, value);
    if (ui >= 0) {
        (void)uinstr_emit(e, uinstr_enc_abc(OP_SETUPVAL, v, (uint8_t)ui, 0U), line);
    } else {
        /* An update, not a declaration: the runtime raises when the name
         * resolves nowhere. */
        uslot_emit(e, OP_SETSLOT_UPDATE, v, fs->r_globals, usite(e, name), line);
        if (fs->parent == NULL) lazy_global_note(e, name, init_mask(value));
    }
    return settle(e, v, want, line);
}

/* --- slots ------------------------------------------------------------------ */

static bool func_has_lazy(const UAstNode *fn) {
    for (int i = 0; i < fn->u.func.param_count; i++) {
        if (fn->u.func.params[i]->u.param.is_lazy) return true;
    }
    return false;
}

/* A method's call site cannot see its signature through dynamic
 * dispatch, so a lazy parameter there would silently be eager. */
static bool reject_lazy_method(UEmitter *e, const UAstNode *value) {
    if (value == NULL || value->kind != AST_FUNCTION || !func_has_lazy(value)) return false;
    urbi_emit_diag_error(e, value, "lazy parameters are not allowed on methods");
    (void)uemit_fail(e, EMIT_LAZY_ON_METHOD);
    return true;
}

static uint8_t ex_member_get(UEmitter *e, const UAstNode *n, int want) {
    uint8_t r = uexpr_any(e, n->u.member.recv);
    const char *name = uemit_intern(e, n->u.member.name_start, n->u.member.name_len);
    if (name == NULL) return 0U;
    uint8_t d = result_reg(e, want, r, r);
    uslot_emit(e, OP_GETSLOT, d, r, usite(e, name), line_of(n));
    return d;
}

static uint8_t ex_member_set(UEmitter *e, const UAstNode *n, int want) {
    if (reject_lazy_method(e, n->u.member.value)) return 0U;
    uint8_t r = uexpr_any(e, n->u.member.recv);
    uint8_t v = uexpr_any(e, n->u.member.value);
    const char *name = uemit_intern(e, n->u.member.name_start, n->u.member.name_len);
    if (name == NULL) return 0U;
    uslot_emit(e, OP_SETSLOT, v, r, usite(e, name), line_of(n));
    return settle(e, v, want, line_of(n));
}

/* --- operators ---------------------------------------------------------- */

static uint8_t ex_unary(UEmitter *e, const UAstNode *n, int want) {
    uint32_t line = line_of(n);
    uint8_t r = uexpr_any(e, n->u.unary.operand);
    uint8_t d = result_reg(e, want, r, r);
    if (n->u.unary.op == UOP_NOT) {
        /* TEST skips the JMP when r is falsy, landing on "true". */
        (void)uinstr_emit(e, uinstr_enc_abc(OP_TEST, r, 0U, 0U), line);
        (void)uinstr_emit(e, uinstr_enc_abx(OP_JMP, 0U, (uint16_t)(UEMIT_JMP_BIAS + 1)), line);
        (void)uinstr_emit(e, uinstr_enc_abc(OP_LOADBOOL, d, 1U, 1U), line);
        (void)uinstr_emit(e, uinstr_enc_abc(OP_LOADBOOL, d, 0U, 0U), line);
        return d;
    }
    (void)uinstr_emit(e, uinstr_enc_abc(OP_NEG, d, r, 0U), line);
    return d;
}

static uint8_t ex_binary(UEmitter *e, const UAstNode *n, int want) {
    uint8_t l = uexpr_any(e, n->u.binary.lhs);
    uint8_t r = uexpr_any(e, n->u.binary.rhs);
    uint8_t d = result_reg(e, want, l, r);
    UOpcode op = n->u.binary.op == BOP_SUB ? OP_SUB
               : n->u.binary.op == BOP_MUL ? OP_MUL
               : n->u.binary.op == BOP_DIV ? OP_DIV
               :                             OP_ADD;
    (void)uinstr_emit(e, uinstr_enc_abc(op, d, l, r), line_of(n));
    return d;
}

static uint8_t ex_compare(UEmitter *e, const UAstNode *n, int want) {
    uint32_t line = line_of(n);
    uint8_t l = uexpr_any(e, n->u.cmp.lhs);
    uint8_t r = uexpr_any(e, n->u.cmp.rhs);
    uint8_t d = result_reg(e, want, l, r);
    /* The compare skips the next instruction when its result differs
     * from A; the skipped JMP lands on "true".  `>` and `>=` swap the
     * operands of `<` and `<=`; `!=` is EQ with the sense flipped. */
    UOpcode op;
    uint8_t a = 0U, b = l, c = r;
    switch (n->u.cmp.op) {
    case CMP_EQ:  op = OP_EQ; break;
    case CMP_NEQ: op = OP_EQ; a = 1U; break;
    case CMP_LT:  op = OP_LT; break;
    case CMP_LE:  op = OP_LE; break;
    case CMP_GT:  op = OP_LT; b = r; c = l; break;
    case CMP_GE:  op = OP_LE; b = r; c = l; break;
    default:      (void)uemit_fail(e, EMIT_UNSUPPORTED_AST); return d;
    }
    (void)uinstr_emit(e, uinstr_enc_abc(op, a, b, c), line);
    (void)uinstr_emit(e, uinstr_enc_abx(OP_JMP, 0U, (uint16_t)(UEMIT_JMP_BIAS + 1)), line);
    (void)uinstr_emit(e, uinstr_enc_abc(OP_LOADBOOL, d, 1U, 1U), line);
    (void)uinstr_emit(e, uinstr_enc_abc(OP_LOADBOOL, d, 0U, 0U), line);
    return d;
}

static uint8_t ex_logical(UEmitter *e, const UAstNode *n, int want) {
    uint32_t line = line_of(n);
    uint8_t d = uemit_target(e, want);
    uint8_t base = e->fs->freereg;
    uint8_t l = uexpr_any(e, n->u.logical.lhs);
    /* TESTSET skips the JMP (and evaluates the right side) when the left
     * side does not settle the result: truthy for &&, falsy for ||.
     * Otherwise it copies the left side into d and the JMP skips the
     * right side. */
    uint8_t c = n->u.logical.is_or ? 0U : 1U;
    (void)uinstr_emit(e, uinstr_enc_abc(OP_TESTSET, d, l, c), line);
    int skip = ujmp_emit(e, line);
    ureg_free_to(e, base);
    uexpr_to(e, n->u.logical.rhs, d);
    ujmp_patch_here(e, skip);
    return d;
}

/* --- calls ------------------------------------------------------------------ */

/* The lazy mask of the binding a call through `name` reaches: the
 * innermost function that declares it as a local, else a chunk-top
 * record.  A binding that shadows without a mask stops the search. */
static uint16_t callee_lazy_mask(const UEmitter *e, const char *name) {
    for (const UFuncState *fs = e->fs; fs != NULL; fs = fs->parent) {
        int li = ulocal_find(fs, name);
        if (li >= 0) return fs->locals[li].lazy_mask;
    }
    for (int i = 0; i < e->nlazy_globals; i++) {
        if (e->lazy_globals[i].name == name) return e->lazy_globals[i].lazy_mask;
    }
    return 0U;
}

/* True when `a` names a lazy parameter of the current function. */
static bool is_lazy_param(UEmitter *e, const UAstNode *a) {
    if (a->kind != AST_IDENT) return false;
    const char *nm = uemit_intern(e, a->u.ident.start, a->u.ident.len);
    if (nm == NULL) return false;
    int li = ulocal_find(e->fs, nm);
    return li >= 0 && (e->fs->locals[li].flags & ULOCAL_LAZY_PARAM) != 0U;
}

/* One call argument into the next frame register.  A lazy parameter
 * passed on as an argument travels as its thunk, unforced, whatever the
 * callee; any other argument to a lazy parameter becomes a
 * zero-parameter function over it. */
static uint8_t call_arg(UEmitter *e, UAstNode *a, bool lazy) {
    if (is_lazy_param(e, a)) {
        bool saved = e->lazy_arg_ctx;
        e->lazy_arg_ctx = true;
        uint8_t t = uexpr_next(e, a);
        e->lazy_arg_ctx = saved;
        return t;
    }
    if (!lazy) return uexpr_next(e, a);
    UAstNode *stmts[1];
    stmts[0] = a;
    UAstNode body;
    urbi_zero(&body, sizeof body);
    body.kind = AST_BLOCK;
    body.line = a->line;
    body.col = a->col;
    body.u.block.stmts = stmts;
    body.u.block.count = 1;
    return uexpr_function(e, NULL, 0, &body, true);
}

static uint8_t ex_call(UEmitter *e, const UAstNode *n, int want) {
    uint32_t line = line_of(n);
    UAstNode *callee = n->u.call.callee;
    int nargs = n->u.call.arg_count;
    if (nargs > 252) {
        urbi_emit_diag_error(e, n, "too many arguments (%d; max 252)", nargs);
        (void)uemit_fail(e, EMIT_TOO_MANY_ARGS);
        return 0U;
    }
    bool method = (callee->kind == AST_MEMBER_GET);
    if (method && nargs >= 3
        && name_is(callee->u.member.name_start, callee->u.member.name_len, "setProperty")
        && reject_lazy_method(e, n->u.call.args[2]))
        return 0U;

    /* The frame starts at the top of the register file: the callee's
     * registers begin right above its arguments.  `want` is reused when
     * it is itself the topmost temporary. */
    const UFuncState *fs = e->fs;
    uint8_t base;
    if (want >= 0 && (int)fs->freereg == want + 1 && want >= (int)ureg_top(e)) base = (uint8_t)want;
    else base = ureg_alloc(e);

    uint8_t argbase;
    uint16_t mask = 0U;
    if (method) {
        (void)ureg_alloc(e);                       /* base + 1: the receiver */
        uint8_t r = uexpr_any(e, callee->u.member.recv);
        const char *name = uemit_intern(e, callee->u.member.name_start, callee->u.member.name_len);
        if (name == NULL) return 0U;
        uslot_emit(e, OP_SELF, base, r, usite(e, name), line);
        ureg_free_to(e, (uint8_t)(base + 2U));
        argbase = 2U;
    } else {
        if (callee->kind == AST_IDENT) {
            const char *name = uemit_intern(e, callee->u.ident.start, callee->u.ident.len);
            if (name == NULL) return 0U;
            mask = callee_lazy_mask(e, name);
        }
        uexpr_to(e, callee, base);
        argbase = 1U;
    }

    for (int i = 0; i < nargs && e->error == EMIT_OK; i++) {
        UAstNode *a = n->u.call.args[i];
        bool lazy = i < 16 && ((mask >> i) & 1U) != 0U;
        uint8_t t = call_arg(e, a, lazy);
        if (e->error == EMIT_OK && t != (uint8_t)(base + argbase + i)) {
            /* Only a declaration inside an argument could leave a local
             * between two arguments. */
            urbi_emit_diag_error(e, a, "a declaration cannot be a call argument");
            (void)uemit_fail(e, EMIT_UNSUPPORTED_AST);
        }
    }
    (void)uinstr_emit(e, uinstr_enc_abc(OP_CALL, base, (uint8_t)(nargs + argbase),
                                        method ? (uint8_t)(UCALL_C_METHOD | 2U) : 2U), line);
    ureg_free_to(e, (uint8_t)(base + 1U));
    return settle(e, base, want, line);
}

/* --- function literals ---------------------------------------------------- */

uint16_t ufunc_lazy_mask(const UAstNode *fn) {
    uint16_t mask = 0U;
    for (int i = 0; i < fn->u.func.param_count && i < 16; i++) {
        if (fn->u.func.params[i]->u.param.is_lazy) mask = (uint16_t)(mask | (1U << i));
    }
    return mask;
}

/* The minimum-arity check and the default fills, reading the passed
 * count from r_nargs:
 *
 *   LOADK t, K(min); LT 0, nargs, t; JMP ok; LOADK t, K(msg); THROW t
 *   ok:
 *   for each defaulted i: LOADK t, K(i); LE 0, nargs, t; JMP skip; <default -> R[i]>
 *   skip:
 */
static void arity_prologue(UEmitter *e, UAstNode **params, int nparams) {
    const UFuncState *fs = e->fs;
    int min_arity = 0;
    for (int i = 0; i < nparams; i++) {
        if (params[i]->u.param.default_expr == NULL) min_arity = i + 1;
    }
    uint32_t line = line_of(params[0]);
    if (min_arity > 0) {
        char msg[64];
        int m = 0;
        for (const char *cp = "function call: wrong argument count (expected "; *cp != '\0'; cp++) msg[m++] = *cp;
        if (min_arity < nparams) {
            for (const char *cp = "at least "; *cp != '\0'; cp++) msg[m++] = *cp;
        }
        char dig[4];
        int nd = 0, v = min_arity;
        do { dig[nd++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0);
        while (nd > 0) msg[m++] = dig[--nd];
        msg[m++] = ')';
        const char *text = uemit_intern(e, msg, m);
        if (text == NULL) return;
        uint16_t kmin = uconst_int(e, (int64_t)min_arity);
        uint16_t kmsg = uconst_str(e, text);
        uint8_t t = ureg_alloc(e);
        (void)uinstr_emit(e, uinstr_enc_abx(OP_LOADK, t, kmin), line);
        (void)uinstr_emit(e, uinstr_enc_abc(OP_LT, 0U, fs->r_nargs, t), line);
        int ok = ujmp_emit(e, line);
        (void)uinstr_emit(e, uinstr_enc_abx(OP_LOADK, t, kmsg), line);
        (void)uinstr_emit(e, uinstr_enc_abc(OP_THROW, t, 0U, 0U), line);
        ujmp_patch_here(e, ok);
        ureg_free_to(e, t);
    }
    for (int i = min_arity; i < nparams; i++) {
        uint32_t dline = line_of(params[i]);
        uint16_t ki = uconst_int(e, (int64_t)i);
        uint8_t t = ureg_alloc(e);
        (void)uinstr_emit(e, uinstr_enc_abx(OP_LOADK, t, ki), dline);
        (void)uinstr_emit(e, uinstr_enc_abc(OP_LE, 0U, fs->r_nargs, t), dline);
        int skip = ujmp_emit(e, dline);
        ureg_free_to(e, t);
        uexpr_to(e, params[i]->u.param.default_expr, (uint8_t)i);
        ujmp_patch_here(e, skip);
    }
}

/* The body of a function: its statements in a block of their own, the
 * last one's value returned (nil when as_expression is false). */
static void function_body(UEmitter *e, UAstNode *body, bool as_expression) {
    uint32_t line = line_of(body);
    if (!ublock_open(e, false)) return;
    UAstNode **stmts = &body;
    int count = 1;
    if (body->kind == AST_BLOCK) { stmts = body->u.block.stmts; count = body->u.block.count; }
    uint8_t r = 0U;
    bool have = false;
    for (int i = 0; i < count; i++) {
        if (i == count - 1 && as_expression) { r = uexpr_any(e, stmts[i]); have = true; }
        else ustmt(e, stmts[i]);
    }
    (void)ublock_close(e);
    if (!have) {
        r = ureg_alloc(e);
        (void)uinstr_emit(e, uinstr_enc_abc(OP_LOADNIL, r, 0U, 0U), line);
    }
    /* The close may have reset freereg below r, but nothing has written
     * the register since: the function returns it right away. */
    (void)uinstr_emit(e, uinstr_enc_abc(OP_RET, r, 0U, 0U), line);
}

/* Compile a function literal into a child proto and leave its closure in
 * `dst`.  Layout of the child's registers: parameters, the passed count
 * (when there are parameters), the realm's globals, then locals. */
static void func_literal(UEmitter *e, UAstNode **params, int nparams, UAstNode *body,
                         bool as_expression, uint8_t dst) {
    if (nparams > UFS_MAX_LOCALS) {
        urbi_emit_diag_error(e, body, "too many parameters (%d; max %d)", nparams, UFS_MAX_LOCALS);
        (void)uemit_fail(e, EMIT_REG_EXHAUSTED);
        return;
    }
    const char *names[UFS_MAX_LOCALS];
    for (int i = 0; i < nparams; i++) {
        names[i] = uemit_intern(e, params[i]->u.param.name_start, params[i]->u.param.name_len);
        if (names[i] == NULL) return;
    }
    UFuncState *parent = e->fs;
    UProto *child = uproto_alloc_nested(e->module, parent->proto);
    if (child == NULL) { (void)uemit_fail(e, EMIT_OOM); return; }
    uint16_t idx = (uint16_t)(parent->proto->nested_count - 1U);
    child->nparams = (uint8_t)nparams;
    child->arity_prologue = 1U;

    UFuncState *fs = ufunc_open(e, child);
    if (fs == NULL) return;
    bool saved_ctx = e->lazy_arg_ctx;
    e->lazy_arg_ctx = false;
    uint32_t line = line_of(body);
    for (int i = 0; i < nparams && e->error == EMIT_OK; i++) {
        int li = ulocal_declare(e, names[i]);
        if (li >= 0 && params[i]->u.param.is_lazy) fs->locals[li].flags |= ULOCAL_LAZY_PARAM;
    }
    if (nparams > 0) fs->r_nargs = upin(e);
    fs->r_globals = upin(e);
    (void)uinstr_emit(e, uinstr_enc_abc(OP_LOAD_REALM_GLOBAL, fs->r_globals, 0U, 0U), line);
    if (nparams > 0) arity_prologue(e, params, nparams);
    function_body(e, body, as_expression);

    UUpval ups[UFS_MAX_UPVALUES];
    int nup = fs->nupvals;
    for (int i = 0; i < nup; i++) ups[i] = fs->upvals[i];
    ufunc_close(e);
    e->lazy_arg_ctx = saved_ctx;

    (void)uinstr_emit(e, uinstr_enc_abx(OP_CLOSURE, dst, idx), line);
    for (int i = 0; i < nup; i++) {
        (void)uinstr_emit(e, uinstr_enc_abc(OP_MOVE, 0U, ups[i].in_stack ? 1U : 0U, ups[i].idx), line);
    }
}

uint8_t uexpr_function(UEmitter *e, UAstNode **params, int nparams,
                       UAstNode *body, bool as_expression) {
    uint8_t t = ureg_alloc(e);
    func_literal(e, params, nparams, body, as_expression, t);
    return t;
}

void uexpr_function_to(UEmitter *e, UAstNode **params, int nparams,
                       UAstNode *body, bool as_expression, uint8_t dst) {
    func_literal(e, params, nparams, body, as_expression, dst);
}

/* --- dispatch ----------------------------------------------------------- */

static uint8_t dispatch(UEmitter *e, UAstNode *n, int want) {
    uint32_t line = line_of(n);
    switch (n->kind) {
    case AST_INT:        return ex_loadk(e, uconst_int(e, n->u.i), want, line);
    case AST_FLOAT_LIT:  return ex_loadk(e, uconst_float(e, n->u.f), want, line);
    case AST_STR: {
        const char *s = uemit_intern(e, n->u.str_lit.bytes, n->u.str_lit.len);
        if (s == NULL) return 0U;
        return ex_loadk(e, uconst_str(e, s), want, line);
    }
    case AST_BOOL: {
        uint8_t d = uemit_target(e, want);
        (void)uinstr_emit(e, uinstr_enc_abc(OP_LOADBOOL, d, n->u.b ? 1U : 0U, 0U), line);
        return d;
    }
    case AST_NIL:
    case AST_NOOP:       return ex_simple(e, OP_LOADNIL, 0U, want, line);
    case AST_THIS:       return ex_this(e, n, want);
    case AST_IDENT:      return ex_ident(e, n, want);
    case AST_VAR_DECL:   return ex_var_decl(e, n, want);
    case AST_ASSIGN:     return ex_assign(e, n, want);
    case AST_MEMBER_GET: return ex_member_get(e, n, want);
    case AST_MEMBER_SET: return ex_member_set(e, n, want);
    case AST_UNARY:      return ex_unary(e, n, want);
    case AST_BINARY:     return ex_binary(e, n, want);
    case AST_COMPARE:    return ex_compare(e, n, want);
    case AST_LOGICAL:    return ex_logical(e, n, want);
    case AST_CALL:       return ex_call(e, n, want);
    case AST_FUNCTION: {
        uint8_t d = uemit_target(e, want);
        func_literal(e, n->u.func.params, n->u.func.param_count, n->u.func.body, true, d);
        return d;
    }
    case AST_SEQ:        return uctrl_seq(e, n, want);
    case AST_BLOCK:      return uctrl_block(e, n, want);
    case AST_IF:         return uctrl_if(e, n, want);
    case AST_WHILE:      return uctrl_while(e, n, want);
    case AST_SWITCH:     return uctrl_switch(e, n, want);
    case AST_BREAK:      return uctrl_break(e, n, want);
    case AST_CONTINUE:   return uctrl_continue(e, n, want);
    case AST_RETURN:     return uctrl_return(e, n, want);
    case AST_THROW:      return uctrl_throw(e, n, want);
    case AST_TRY:        return uscope_try(e, n, want);
    case AST_TAG_PREFIX: return uscope_tag(e, n, want);
    case AST_WATCHER:    return uwatch_install_node(e, n, want);
    case AST_ERROR:      (void)uemit_fail(e, EMIT_AST_ERROR); return 0U;
    case AST_PARAM:      break;
    }
    (void)uemit_fail(e, EMIT_UNSUPPORTED_AST);
    return 0U;
}

static uint8_t uexpr_want(UEmitter *e, UAstNode *n, int want) {
    if (e->error != EMIT_OK) return want >= 0 ? (uint8_t)want : 0U;
    uint8_t entry = e->fs->freereg;
    const UAstNode *outer = e->cur_node;
    e->cur_node = n;
    uint8_t r = dispatch(e, n, want);
    e->cur_node = outer;
    if (e->error != EMIT_OK) return want >= 0 ? (uint8_t)want : 0U;
    /* Release what the arm used: everything above `want`'s caller frame,
     * or everything above the value when it is a temporary. */
    if (want < 0 && r >= ureg_top(e)) ureg_free_to(e, (uint8_t)(r + 1U));
    else ureg_free_to(e, entry);
    return r;
}

uint8_t uexpr_any(UEmitter *e, UAstNode *n) {
    return uexpr_want(e, n, -1);
}

void uexpr_to(UEmitter *e, UAstNode *n, uint8_t dst) {
    (void)uexpr_want(e, n, (int)dst);
}

uint8_t uexpr_next(UEmitter *e, UAstNode *n) {
    uint8_t t = ureg_alloc(e);
    (void)uexpr_want(e, n, (int)t);
    return t;
}

void ustmt(UEmitter *e, UAstNode *n) {
    uint8_t entry = e->fs->freereg;
    (void)uexpr_want(e, n, -1);
    ureg_free_to(e, entry);
}
