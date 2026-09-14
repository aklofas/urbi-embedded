/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/uexec_ops.c — the bytecode dispatch loop.
 *
 * Scope: the sequential subset of the 50-opcode set plus the cleanup
 * stack (TRY_BEGIN, TRY_END, THROW, RESUME, LOAD_CATCH_VALUE, PUSH_TAG,
 * POP_TAG), whose walker lives in rt/uunwind.c.  Concurrency (FORK_*,
 * JOIN_WAIT, TAG_STOP) and the reactive installs land with their own
 * tasks; every opcode not handled here throws through `default:` rather
 * than silently doing nothing.
 *
 * The tag scope OP_PUSH_TAG opens is a cleanup entry and nothing more
 * until the scheduler task gives it a tag object, an onleave body and a
 * stop to match -- enough for a try nested inside a tagged block to
 * unwind correctly, which is what the unwinder needs from it.
 *
 * Register addressing: R is recomputed from s->stack + f->base after
 * anything that can grow the stack, and f is re-fetched from
 * s->frames[s->nframes - 1] for the same reason (both arrays are
 * reallocated in place by ustrand_ensure_stack / the frame grower). */

#include "rt/uexec.h"
#include "chunk/uchunk.h"
#include "emit/ufront.h"

#define OPA(i)  uinstr_a(i)
#define OPB(i)  uinstr_b(i)
#define OPC(i)  uinstr_c(i)
#define OPBX(i) uinstr_bx(i)

#define UEXEC_JMP_BIAS 32768

/* --- message assembly ---------------------------------------------------
 *
 * src/rt has no <stdio.h> by policy, so diagnostics are concatenated.
 * Both helpers always NUL-terminate and return the new length. */

static size_t uexec_put(char *buf, size_t cap, size_t at, const char *s)
{
    while (*s && at + 1 < cap) buf[at++] = *s++;
    buf[at] = '\0';
    return at;
}

static size_t uexec_put_u8(char *buf, size_t cap, size_t at, uint8_t n)
{
    char tmp[4];
    size_t k = 0;
    do { tmp[k++] = (char)('0' + (n % 10u)); n = (uint8_t)(n / 10u); } while (n);
    while (k && at + 1 < cap) buf[at++] = tmp[--k];
    buf[at] = '\0';
    return at;
}

/* The operand-type names the legacy diagnostics use.  Anything that is
 * not one of the five atom kinds reads as "unknown", which is what the
 * corpus pins for a user object. */
static const char *uexec_kind_name(UValue v)
{
    switch (v.kind) {
    case UV_NIL:   return "Nil";
    case UV_INT:   return "Integer";
    case UV_FLOAT: return "Float";
    case UV_BOOL:  return "Bool";
    case UV_STR: case UV_SYM: return "String";
    default:       return "unknown";
    }
}

/* --- slot helpers ------------------------------------------------------ */

/* Reads one resolved slot, running a getter when the slot carries one.
 * Returns UEXEC_OK or UEXEC_THROW. */
static int slot_read(UVM *vm, UStrand *s, const UObjSlotRef *ref, UValue recv, UValue *out)
{
    UValue raw = uobj_slot_value(ref);
    uint8_t attrs = uobj_slot_attrs(ref);
    if ((attrs & (USLOT_GETTER | USLOT_SETTER)) == 0) { *out = raw; return UEXEC_OK; }
    UProps *pr = (UProps *)raw.v.p;
    if ((attrs & USLOT_GETTER) && pr->getter.kind == UV_CELL) {
        UClosure *g = (UClosure *)pr->getter.v.p;
        return uexec_call(vm, s, g, recv, NULL, 0, out);
    }
    *out = pr->value;
    return UEXEC_OK;
}

static int slot_get(UVM *vm, UStrand *s, UValue recv, const USym *name, const char *what, UValue *out)
{
    UObject *o = uv_dispatch_proto(vm, recv);
    if (o == NULL) {
        char msg[96]; size_t at = 0;
        const char *p = what; while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
        p = ": receiver is not an Object"; while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
        msg[at] = '\0';
        return uexec_throw(vm, s, UP_TYPEERROR, msg);
    }
    UObjSlotRef ref;
    if (!uobj_resolve(vm, o, name, &ref)) {
        char msg[160]; size_t at = 0;
        const char *p = what; while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
        p = ": slot '"; while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
        p = name->bytes; while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
        p = "' not found"; while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
        msg[at] = '\0';
        return uexec_throw(vm, s, UP_LOOKUPERROR, msg);
    }
    return slot_read(vm, s, &ref, recv, out);
}

/* --- arithmetic -------------------------------------------------------- */

static bool str_kind(UValue v) { return v.kind == UV_STR || v.kind == UV_SYM; }

/* Resolves `name` on the left operand's prototype and calls it with one
 * argument.  Returns 1 when the slot did not exist (caller falls through
 * to its own error), 0 on a completed call, or UEXEC_THROW.
 *
 * The lookup goes through uv_dispatch_proto rather than requiring a UV_OBJ
 * receiver, because a List is a cell: `[1, 2] + [3]` has to find List's
 * `+` slot the same way a user class finds the one it declared.  An atom
 * resolves against its own prototype and finds nothing, which is the same
 * fall-through as before. */
static int object_binop(UVM *vm, UStrand *s, UValue lhs, UValue rhs, const char *name, UValue *out)
{
    UObject *recv_proto = uv_dispatch_proto(vm, lhs);
    if (!recv_proto) return 1;
    const USym *sym = usym_cstr(vm, name);
    if (!sym) return uexec_throw(vm, s, UP_OOMERROR, "out of memory interning an operator name");
    UObjSlotRef ref;
    if (!uobj_resolve(vm, recv_proto, sym, &ref)) return 1;
    UValue fn = uv_nil();
    int rc = slot_read(vm, s, &ref, lhs, &fn);
    if (rc != UEXEC_OK) return rc;
    if (fn.kind != UV_CELL || ((UCell *)fn.v.p)->type != UCELL_CLOSURE) return 1;
    return uexec_call(vm, s, (UClosure *)fn.v.p, lhs, &rhs, 1, out);
}

/* The operator's user-facing phrase, as uopcodes.def spells it. */
static const char *arith_op_name(uint8_t op)
{
    switch (op) {
    case OP_ADD: return "'+'";
    case OP_SUB: return "'-'";
    case OP_MUL: return "'*'";
    case OP_LT:  return "'<'";
    case OP_LE:  return "'<='";
    default:     return "'/'";
    }
}

/* "<op> operands must be Integer or Float (got <Kind>, <Kind>)", carrying
 * the source position -- the legacy shape, pinned by
 * the operator fixtures under tests/chk.  The wording names only the numeric kinds
 * even though String + String and String < String have their own fast
 * paths, because that is the text the corpus fixed. */
static int binop_type_error(UVM *vm, UStrand *s, uint8_t op, UValue b, UValue c)
{
    char msg[128]; size_t at = 0;
    at = uexec_put(msg, sizeof msg, at, arith_op_name(op));
    at = uexec_put(msg, sizeof msg, at, " operands must be Integer or Float (got ");
    at = uexec_put(msg, sizeof msg, at, uexec_kind_name(b));
    at = uexec_put(msg, sizeof msg, at, ", ");
    at = uexec_put(msg, sizeof msg, at, uexec_kind_name(c));
    (void)uexec_put(msg, sizeof msg, at, ")");
    return uexec_throw_here(vm, s, UP_TYPEERROR, msg);
}

static int arith(UVM *vm, UStrand *s, uint8_t op, UValue b, UValue c, UValue *out)
{
    if (b.kind == UV_INT && c.kind == UV_INT) {
        int64_t r;
        switch (op) {
        case OP_ADD: if (!__builtin_add_overflow(b.v.i, c.v.i, &r)) { *out = uv_int(r); return UEXEC_OK; } break;
        case OP_SUB: if (!__builtin_sub_overflow(b.v.i, c.v.i, &r)) { *out = uv_int(r); return UEXEC_OK; } break;
        case OP_MUL: if (!__builtin_mul_overflow(b.v.i, c.v.i, &r)) { *out = uv_int(r); return UEXEC_OK; } break;
        default:
            if (c.v.i == 0) return uexec_throw_here(vm, s, UP_DIVBYZERO, "division by 0");
            *out = uv_float((double)b.v.i / (double)c.v.i);
            return UEXEC_OK;
        }
        /* Overflow on + - * promotes to Float rather than wrapping. */
        *out = uv_float(op == OP_ADD ? (double)b.v.i + (double)c.v.i
                      : op == OP_SUB ? (double)b.v.i - (double)c.v.i
                                     : (double)b.v.i * (double)c.v.i);
        return UEXEC_OK;
    }
    if (uv_is_number(b) && uv_is_number(c)) {
        double x = uv_as_double(b), y = uv_as_double(c);
        switch (op) {
        case OP_ADD: *out = uv_float(x + y); return UEXEC_OK;
        case OP_SUB: *out = uv_float(x - y); return UEXEC_OK;
        case OP_MUL: *out = uv_float(x * y); return UEXEC_OK;
        default:
            if (y == 0.0) return uexec_throw_here(vm, s, UP_DIVBYZERO, "division by 0");
            *out = uv_float(x / y); return UEXEC_OK;
        }
    }
    if (op == OP_ADD && str_kind(b) && str_kind(c)) {
        uint32_t nb, nc;
        const char *pb = uv_str_bytes(b, &nb);
        const char *pc = uv_str_bytes(c, &nc);
        /* Root both operands: ustr_concat allocates and may collect while
         * pb/pc still point into the source cells. */
        UValue rb = b, rc2 = c;
        USTRAND_ROOT(s, rb); USTRAND_ROOT(s, rc2);
        UStr *r = ustr_concat(vm, pb, nb, pc, nc);
        /* Unrooting in reverse order writes s->croots twice in a row and
         * the first write is dead, but the pairing is the macro protocol. */
        /* cppcheck-suppress redundantAssignment */
        USTRAND_UNROOT(s, rc2); USTRAND_UNROOT(s, rb);
        if (!r) return uexec_throw(vm, s, UP_OOMERROR, "out of memory concatenating strings");
        *out = uv_str(r);
        return UEXEC_OK;
    }
    {
        /* object_binop takes the bare glyph; arith_op_name quotes it for
         * the diagnostic, so the slot name is spelled here. */
        const char *slot = (op == OP_ADD) ? "+" : (op == OP_SUB) ? "-" : (op == OP_MUL) ? "*" : "/";
        int rc = object_binop(vm, s, b, c, slot, out);
        if (rc != 1) return rc;
    }
    return binop_type_error(vm, s, op, b, c);
}

/* Unary minus.  Numbers negate directly; anything else gets one chance at
 * a zero-argument "-" slot on its prototype (the unary counterpart of
 * object_binop; the neg operator fixture pins the overload) before
 * the legacy diagnostic. */
static int negate(UVM *vm, UStrand *s, UValue v, UValue *out)
{
    if (v.kind == UV_INT) {
        int64_t r;
        if (__builtin_sub_overflow((int64_t)0, v.v.i, &r)) *out = uv_float(-(double)v.v.i);
        else *out = uv_int(r);
        return UEXEC_OK;
    }
    if (v.kind == UV_FLOAT) { *out = uv_float(-v.v.f); return UEXEC_OK; }

    UObject *proto = uv_dispatch_proto(vm, v);
    if (proto) {
        const USym *sym = usym_cstr(vm, "-");
        UObjSlotRef ref;
        if (sym && uobj_resolve(vm, proto, sym, &ref)) {
            UValue fn = uv_nil();
            int rc = slot_read(vm, s, &ref, v, &fn);
            if (rc != UEXEC_OK) return rc;
            if (fn.kind == UV_CELL && ((UCell *)fn.v.p)->type == UCELL_CLOSURE)
                return uexec_call(vm, s, (UClosure *)fn.v.p, v, NULL, 0, out);
        }
    }
    char msg[128]; size_t at = 0;
    at = uexec_put(msg, sizeof msg, at, "unary '-' operand must be Integer or Float (got ");
    at = uexec_put(msg, sizeof msg, at, uexec_kind_name(v));
    (void)uexec_put(msg, sizeof msg, at, ")");
    return uexec_throw_here(vm, s, UP_TYPEERROR, msg);
}

/* --- comparison -------------------------------------------------------- */

static int compare_lt_le(UVM *vm, UStrand *s, uint8_t op, UValue b, UValue c, bool *out)
{
    if (uv_is_number(b) && uv_is_number(c)) {
        double x = uv_as_double(b), y = uv_as_double(c);
        *out = (op == OP_LT) ? (x < y) : (x <= y);
        return UEXEC_OK;
    }
    if (str_kind(b) && str_kind(c)) {
        uint32_t nb, nc;
        const char *pb = uv_str_bytes(b, &nb);
        const char *pc = uv_str_bytes(c, &nc);
        uint32_t n = nb < nc ? nb : nc;
        int d = n ? memcmp(pb, pc, n) : 0;
        if (d == 0) d = (nb < nc) ? -1 : (nb > nc) ? 1 : 0;
        *out = (op == OP_LT) ? (d < 0) : (d <= 0);
        return UEXEC_OK;
    }
    {
        UValue r = uv_nil();
        int rc = object_binop(vm, s, b, c, op == OP_LT ? "<" : "<=", &r);
        if (rc == UEXEC_OK) { *out = uv_truthy(r); return UEXEC_OK; }
        if (rc != 1) return rc;
    }
    /* Mirror-operand retry.  The emitter compiles `a > b` as OP_LT(b, a),
     * so a `>` overload declared on the SYNTACTIC left operand arrives
     * here as the right one.  On a miss, ask the right operand for the
     * mirrored operator with the arguments swapped back -- which is what
     * `a > b` literally means. */
    {
        UValue r = uv_nil();
        int rc = object_binop(vm, s, c, b, op == OP_LT ? ">" : ">=", &r);
        if (rc == UEXEC_OK) { *out = uv_truthy(r); return UEXEC_OK; }
        if (rc != 1) return rc;
    }
    return binop_type_error(vm, s, op, b, c);
}

static int compare_eq(UVM *vm, UStrand *s, UValue b, UValue c, bool *out)
{
    if (b.kind == UV_OBJ) {
        UValue r = uv_nil();
        int rc = object_binop(vm, s, b, c, "==", &r);
        if (rc == UEXEC_OK) { *out = uv_truthy(r); return UEXEC_OK; }
        if (rc != 1) return rc;
    }
    *out = uv_equal(b, c);
    return UEXEC_OK;
}

/* --- calls -------------------------------------------------------------- */

/* "<name>: expected N..M arguments, got K", or the same without the name
 * when the callee has none (a script function literal carries no name
 * through the wire format).  A single accepted count prints as "N"
 * rather than "N..N". */
static int arity_error(UVM *vm, UStrand *s, const char *name,
                       uint8_t lo, uint8_t hi, uint8_t got)
{
    char msg[128];
    size_t at = 0;
    if (name) {
        at = uexec_put(msg, sizeof msg, at, name);
        at = uexec_put(msg, sizeof msg, at, ": ");
    }
    at = uexec_put(msg, sizeof msg, at, "expected ");
    at = uexec_put_u8(msg, sizeof msg, at, lo);
    if (hi != lo) {
        at = uexec_put(msg, sizeof msg, at, hi == UCLOSURE_ANY_ARGS ? " or more" : "..");
        if (hi != UCLOSURE_ANY_ARGS) at = uexec_put_u8(msg, sizeof msg, at, hi);
    }
    at = uexec_put(msg, sizeof msg, at, " argument");
    if (lo != 1 || hi != 1) at = uexec_put(msg, sizeof msg, at, "s");
    at = uexec_put(msg, sizeof msg, at, ", got ");
    (void)uexec_put_u8(msg, sizeof msg, at, got);
    return uexec_throw(vm, s, UP_ARITYERROR, msg);
}

/* A bytecode closure with default parameters checks its own MINIMUM in
 * an emitted prologue (uemit_stmt.c), so all the VM can say is that too
 * many arrived; without defaults it owns both bounds. */
static int arity_error_proto(UVM *vm, UStrand *s, const UClosure *cl, uint8_t got)
{
    const UProto *p = cl->proto;
    char msg[128];
    size_t at = 0;
    if (cl->name) {
        at = uexec_put(msg, sizeof msg, at, cl->name);
        at = uexec_put(msg, sizeof msg, at, ": ");
    }
    /* With no parameters the relaxed and exact checks coincide, so the
     * "at most" hedge would be noise. */
    at = uexec_put(msg, sizeof msg, at,
                   (p->arity_prologue && p->nparams > 0) ? "expected at most " : "expected ");
    at = uexec_put_u8(msg, sizeof msg, at, p->nparams);
    at = uexec_put(msg, sizeof msg, at, p->nparams == 1 ? " argument, got " : " arguments, got ");
    (void)uexec_put_u8(msg, sizeof msg, at, got);
    return uexec_throw(vm, s, UP_ARITYERROR, msg);
}

/* Executes one OP_CALL from frame index `fi`.  Either runs a native
 * inline (result written straight into R[A]) or pushes a bytecode frame.
 * Returns UEXEC_OK or UEXEC_THROW. */
static int do_call(UVM *vm, UStrand *s, uint16_t fi, uint32_t instr)
{
    uint8_t a = OPA(instr), b = OPB(instr), c = OPC(instr);
    bool is_method = (c & 0x80u) != 0;
    int nargs_i = is_method ? (int)b - 2 : (int)b - 1;
    uint8_t arg_off = is_method ? 2u : 1u;
    if (nargs_i < 0) return uexec_throw(vm, s, UP_TYPEERROR, "function call: malformed argument count");
    uint8_t nargs = (uint8_t)nargs_i;

    uint32_t base = s->frames[fi].base;
    UValue callee_v = s->stack[base + a];
    if (callee_v.kind != UV_CELL || ((UCell *)callee_v.v.p)->type != UCELL_CLOSURE)
        return uexec_throw(vm, s, UP_TYPEERROR, "function call: callee is not a closure");
    UClosure *callee = (UClosure *)callee_v.v.p;
    UValue self_value = is_method ? s->stack[base + a + 1u] : uv_nil();

    if (callee->native) {
        if (nargs < callee->min_args || nargs > callee->max_args)
            return arity_error(vm, s, callee->name, callee->min_args, callee->max_args, nargs);
        UValue out = uv_nil();
        UValue self = self_value;
        USTRAND_ROOT(s, out); USTRAND_ROOT(s, self);
        UValue *args = nargs ? &s->stack[base + a + arg_off] : NULL;
        int rc = callee->native(vm, self, args, nargs, &out);
        /* Unrooting in reverse order writes s->croots twice in a row and
         * the first write is dead, but the pairing is the macro protocol. */
        /* cppcheck-suppress redundantAssignment */
        USTRAND_UNROOT(s, self); USTRAND_UNROOT(s, out);
        if (rc != UEXEC_OK) return rc;
        /* The native may have grown the stack through a nested call, so
         * the destination is re-addressed from the (stable) frame base. */
        s->stack[s->frames[fi].base + a] = out;
        return UEXEC_OK;
    }

    UProto *p = callee->proto;
    if (p->arity_prologue ? (nargs > p->nparams) : (nargs != p->nparams))
        return arity_error_proto(vm, s, callee, nargs);

    uint32_t new_base = base + a + arg_off;
    UValue clv = callee_v;
    USTRAND_ROOT(s, clv);
    int prc = ustrand_push_frame_args(s, callee, self_value, new_base, a, nargs);
    USTRAND_UNROOT(s, clv);
    if (prc != 0) return uexec_throw(vm, s, UP_OOMERROR, "function call: out of memory pushing a call frame");
    if (p->arity_prologue && p->nparams > 0) s->stack[new_base + p->nparams] = uv_int((int64_t)nargs);
    return UEXEC_OK;
}

/* The fork opcodes take a closure thunk the emitter built for the arm.
 * A native closure has no bytecode to run on a strand of its own, so it
 * is rejected here rather than deep inside usched_spawn. */
static bool fork_closure(UValue v)
{
    if (v.kind != UV_CELL || ((const UCell *)v.v.p)->type != UCELL_CLOSURE) return false;
    return ((const UClosure *)v.v.p)->proto != NULL;
}

/* --- the dispatch loop --------------------------------------------------- */

static int uexec_run_inner(UVM *vm, UStrand *s, uint32_t budget)
{
    bool unbounded = (budget == 0);
    s->state = USTRAND_RUNNING;
    /* The safepoint a cross-strand stop is consumed at.  utag_stop only
     * marks (`unwind = STOP`, the tag in `transfer`) and wakes; the
     * marked strand walks its own cleanup stack here, the first time the
     * scheduler hands it back to dispatch.  Stop never runs on another
     * strand's stack. */
    if (s->unwind != UUNWIND_NONE && uexec_unwind(vm, s) != 0) return s->state;
    for (;;) {
        if (s->nframes == 0) {
            /* Nothing to dispatch.  Reachable only if a caller enters with
             * an empty strand or the unwinder leaves one behind; both are
             * bugs, so trap in debug builds and die cleanly otherwise
             * rather than indexing frames[(uint16_t)-1]. */
            UGC_ASSERT(0);
            s->state = USTRAND_DEAD;
            return s->state;
        }
        UFrame *f = &s->frames[s->nframes - 1];
        UValue *R = s->stack + f->base;
        const UValue *K = f->closure->proto->constants;
        uint32_t i = *f->pc++;
        switch (i & 0xFFu) {

        case OP_LOADK:    R[OPA(i)] = K[OPBX(i)]; break;
        case OP_MOVE:     R[OPA(i)] = R[OPB(i)]; break;
        case OP_LOADNIL:  R[OPA(i)] = uv_nil(); break;
        case OP_LOADVOID: R[OPA(i)] = uv_void(); break;
        case OP_LOADBOOL:
            R[OPA(i)] = uv_bool(OPB(i) != 0);
            if (OPC(i)) f->pc++;
            break;

        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: {
            UValue res = uv_nil();
            if (arith(vm, s, (uint8_t)(i & 0xFFu), R[OPB(i)], R[OPC(i)], &res) != UEXEC_OK) goto unwind;
            f = &s->frames[s->nframes - 1];
            s->stack[f->base + OPA(i)] = res;
            break;
        }

        case OP_NEG: {
            UValue res = uv_nil();
            if (negate(vm, s, R[OPB(i)], &res) != UEXEC_OK) goto unwind;
            f = &s->frames[s->nframes - 1];
            s->stack[f->base + OPA(i)] = res;
            break;
        }

        case OP_RET: {
            UValue rv = R[OPA(i)];
            /* `return` out of a try is a bare OP_RET -- the emitter plants
             * scope crossings for break/continue but not for return -- so a
             * frame that still owns cleanup entries hands the return to the
             * walker, which runs the finallys and completes it. */
            if (s->ncleanup > 0 && s->cleanup[s->ncleanup - 1].frame >= (uint16_t)(s->nframes - 1)) {
                s->transfer = rv;
                s->unwind = UUNWIND_RETURN;
                goto unwind;
            }
            if (uexec_return(vm, s, rv) != 0) return s->state;
            break;
        }

        case OP_YIELD:
            /* A strand running synchronously on a spare has no scheduler
             * to hand control back to, so yielding is a no-op for it; a
             * scheduled strand goes READY at the queue tail. */
            if (!s->is_spare) { s->state = USTRAND_READY; return s->state; }
            break;

        case OP_GETUPVAL: {
            UClosure *cl = f->closure;
            if (OPB(i) >= cl->nupvals || cl->upvals[OPB(i)] == NULL) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "upvalue read: index out of range");
                goto unwind;
            }
            R[OPA(i)] = *cl->upvals[OPB(i)]->ptr;
            break;
        }
        case OP_SETUPVAL: {
            UClosure *cl = f->closure;
            if (OPB(i) >= cl->nupvals || cl->upvals[OPB(i)] == NULL) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "upvalue write: index out of range");
                goto unwind;
            }
            *cl->upvals[OPB(i)]->ptr = R[OPA(i)];
            break;
        }

        case OP_CLOSURE: {
            UProto *parent = f->closure->proto;
            uint16_t bx = OPBX(i);
            if (parent->nested == NULL || (size_t)bx >= parent->nested_count) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "closure creation: proto index out of range");
                goto unwind;
            }
            UProto *child = parent->nested[bx];
            UClosure *cl = uclosure_new(vm, child, child->nupvals);
            if (!cl) {
                (void)uexec_throw(vm, s, UP_OOMERROR, "closure creation: out of memory");
                goto unwind;
            }
            /* Publish into the destination register before capturing:
             * opening an upvalue allocates, and a register is a root. */
            f = &s->frames[s->nframes - 1];
            R = s->stack + f->base;
            R[OPA(i)] = uv_ptr(UV_CELL, cl);
            if (vm->protos[UP_CLOSURE]) cl->proto_obj = vm->protos[UP_CLOSURE];
            for (uint8_t k = 0; k < child->nupvals; k++) {
                uint32_t pseudo = *f->pc++;
                uint8_t in_stack = OPB(pseudo), src = OPC(pseudo);
                if (in_stack) {
                    UUpval *u = ustrand_find_or_open_upval(s, f->base + src);
                    if (!u) {
                        (void)uexec_throw(vm, s, UP_OOMERROR, "closure creation: out of memory opening an upvalue");
                        goto unwind;
                    }
                    cl->upvals[k] = u;
                    f = &s->frames[s->nframes - 1];   /* the open may have grown the stack */
                } else {
                    UClosure *par = f->closure;
                    if (src >= par->nupvals) {
                        (void)uexec_throw(vm, s, UP_TYPEERROR, "closure creation: upvalue re-capture out of range");
                        goto unwind;
                    }
                    cl->upvals[k] = par->upvals[src];
                }
            }
            break;
        }

        case OP_CLOSE: ustrand_close_upvals(s, f->base + OPA(i)); break;

        case OP_CALL: {
            if (do_call(vm, s, (uint16_t)(s->nframes - 1), i) != UEXEC_OK) goto unwind;
            if (s->state != USTRAND_RUNNING) return s->state;
            break;
        }

        case OP_JMP: {
            int off = (int)OPBX(i) - UEXEC_JMP_BIAS;
            /* Forward offsets are encoded relative to the instruction
             * AFTER the jump; backward offsets relative to the jump
             * itself (the emitter's two encoders, and the old dispatch's
             * safepoint path that skipped the implicit pc++). */
            if (off >= 0) {
                f->pc += off;
            } else {
                f->pc += off - 1;
                if (!unbounded && --budget == 0) { s->state = USTRAND_READY; return s->state; }
            }
            break;
        }

        case OP_TEST:
            if ((int)uv_truthy(R[OPA(i)]) == (int)OPC(i)) f->pc++;
            break;
        case OP_TESTSET:
            if ((int)uv_truthy(R[OPB(i)]) == (int)OPC(i)) f->pc++;
            else R[OPA(i)] = R[OPB(i)];
            break;

        case OP_EQ: {
            bool eq = false;
            if (compare_eq(vm, s, R[OPB(i)], R[OPC(i)], &eq) != UEXEC_OK) goto unwind;
            f = &s->frames[s->nframes - 1];
            if ((int)eq != (int)OPA(i)) f->pc++;
            break;
        }
        case OP_LT: case OP_LE: {
            bool r = false;
            if (compare_lt_le(vm, s, (uint8_t)(i & 0xFFu), R[OPB(i)], R[OPC(i)], &r) != UEXEC_OK) goto unwind;
            f = &s->frames[s->nframes - 1];
            if ((int)r != (int)OPA(i)) f->pc++;
            break;
        }

        case OP_LOAD_REALM_GLOBAL: {
            UObject *g = (s->realm && s->realm->globals) ? s->realm->globals : NULL;
            /* Exactly one strand legitimately has no realm: the one
             * uboot_init runs the stdlib blob on, before any realm
             * exists.  Its globals ARE the shared root object, which is
             * where the overlay's top-level `var`s belong.  After boot a
             * realm-less strand is a bug -- a freed realm still held by a
             * strand, say -- and must not silently write into the object
             * every realm inherits. */
            if (g == NULL && !vm->stdlib_booted) g = vm->root_globals;
            if (g == NULL) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "global access: strand has no realm");
                goto unwind;
            }
            R[OPA(i)] = uv_obj(g);
            break;
        }
        case OP_LOAD_RECV: R[OPA(i)] = f->recv; break;

        case OP_GETSLOT: case OP_SELF: {
            USym **names = uproto_names(f->closure->proto);
            if (names == NULL || OPC(i) >= f->closure->proto->ic_count) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "slot access: no name table bound");
                goto unwind;
            }
            UValue recv = R[OPB(i)];
            UValue out = uv_nil();
            const char *what = ((i & 0xFFu) == OP_SELF) ? "method call" : "slot access";
            if (slot_get(vm, s, recv, names[OPC(i)], what, &out) != UEXEC_OK) goto unwind;
            f = &s->frames[s->nframes - 1];
            R = s->stack + f->base;
            if ((i & 0xFFu) == OP_SELF) R[OPA(i) + 1u] = recv;   /* receiver first — dst may alias recv */
            R[OPA(i)] = out;
            break;
        }

        case OP_SETSLOT: case OP_SETSLOT_UPDATE: {
            USym **names = uproto_names(f->closure->proto);
            if (names == NULL || OPC(i) >= f->closure->proto->ic_count) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "slot write: no name table bound");
                goto unwind;
            }
            UValue recv = R[OPB(i)];
            if (recv.kind != UV_OBJ) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "slot write: receiver is not an Object");
                goto unwind;
            }
            UObject *o = (UObject *)recv.v.p;
            /* UPDATE is the bare-name write, `x = 1`.  It rebinds an
             * existing name and never declares one, so a name that
             * resolves nowhere on the chain is a LookupError rather than
             * a silent new global -- which is what makes a typo in an
             * assignment reportable.  `var x = 1` and the explicit
             * `Realm.x = 1` both emit plain SETSLOT and still create. */
            if ((i & 0xFFu) == OP_SETSLOT_UPDATE) {
                UObjSlotRef probe;
                if (!uobj_resolve(vm, o, names[OPC(i)], &probe)) {
                    char msg[160]; size_t at = 0;
                    const char *p = "slot write: slot '";
                    while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
                    p = names[OPC(i)]->bytes;
                    while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
                    p = "' not found";
                    while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
                    msg[at] = '\0';
                    (void)uexec_throw(vm, s, UP_LOOKUPERROR, msg);
                    goto unwind;
                }
            }
            if (o->cell.flags & UOBJ_F_READONLY) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "slot write: receiver is read-only");
                goto unwind;
            }
            USym *name = names[OPC(i)];
            int idx = uobj_find_local(o, name);
            if (idx >= 0 && (o->attrs[idx] & USLOT_CONSTANT)) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "slot write: slot is constant");
                goto unwind;
            }
            if (idx >= 0 && (o->attrs[idx] & (USLOT_GETTER | USLOT_SETTER))) {
                UProps *pr = (UProps *)o->values[idx].v.p;
                if ((o->attrs[idx] & USLOT_SETTER) && pr->setter.kind == UV_CELL) {
                    UValue arg = R[OPA(i)], ignored = uv_nil();
                    if (uexec_call(vm, s, (UClosure *)pr->setter.v.p, recv, &arg, 1, &ignored) != UEXEC_OK) goto unwind;
                } else {
                    pr->value = R[OPA(i)];
                }
                break;
            }
            if (uobj_set_local(vm, o, name, R[OPA(i)], 0) < 0) {
                (void)uexec_throw(vm, s, UP_OOMERROR, "slot write: out of memory");
                goto unwind;
            }
            break;
        }

        case OP_THROW:
            (void)uexec_throw_value(vm, s, R[OPA(i)]);
            goto unwind;

        /* --- concurrency ------------------------------------------------
         *
         * All three spawn through usched_spawn, the one spawn path: the
         * child lands in the parent's realm under the parent's ambient
         * tag, with the parent's receiver, so a forked arm resolves
         * `Realm.x` and `this` exactly as the code around it does. */

        case OP_FORK_DETACH: {
            UValue cv = R[OPA(i)];
            if (!fork_closure(cv)) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "',' (parallel fork): operand is not a closure");
                goto unwind;
            }
            if (usched_spawn(vm, s->realm, (UClosure *)cv.v.p, s->tag, f->recv, NULL, 0) == NULL) {
                (void)uexec_throw(vm, s, UP_OOMERROR, "',' (parallel fork): cannot spawn the child strand");
                goto unwind;
            }
            break;
        }

        case OP_FORK_JOIN: {
            UValue cv = R[OPA(i)];
            if (!fork_closure(cv)) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "'&' (parallel join): operand is not a closure");
                goto unwind;
            }
            UStrand *child = usched_spawn(vm, s->realm, (UClosure *)cv.v.p, s->tag, f->recv, NULL, 0);
            if (child == NULL) {
                (void)uexec_throw(vm, s, UP_OOMERROR, "'&' (parallel join): cannot spawn the child strand");
                goto unwind;
            }
            /* The spawn allocated: both arrays may have moved. */
            f = &s->frames[s->nframes - 1];
            s->stack[f->base + OPB(i)] = uv_ptr(UV_CELL, child);
            break;
        }

        case OP_JOIN_WAIT: {
            UValue cv = R[OPA(i)];
            if (cv.kind != UV_CELL || ((UCell *)cv.v.p)->type != UCELL_STRAND) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "'&' (parallel join): join operand is not a strand");
                goto unwind;
            }
            UStrand *child = (UStrand *)cv.v.p;
            if (child->state == USTRAND_DEAD) break;      /* already finished */
            if (usched_park(s, &child->joiners, 0) == 0) return s->state;
            /* No scheduler to hand control back to (a spare strand, or
             * inside a synchronous call): the join still has to wait, so
             * the child runs nested on this stack instead. */
            usched_run_inline(vm, child);
            break;
        }

        /* --- the cleanup stack (see rt/uunwind.c for the layouts) ------ */

        case OP_TRY_BEGIN: {
            UCleanup c;
            c.kind = (uint8_t)UCLEAN_TRY;
            c.flags = OPA(i);              /* HAS_CATCH / HAS_FINALLY, as emitted */
            c.saved_unwind = (uint8_t)UUNWIND_NONE;
            c.frame = (uint16_t)(s->nframes - 1);
            c.handler_pc = OPBX(i);        /* absolute instruction index */
            c.onleave_pc = 0;
            c.tag = NULL;
            c.saved = uv_nil();
            if (ustrand_push_cleanup(s, c) != 0) {
                (void)uexec_throw(vm, s, UP_OOMERROR, "try begin: out of memory growing the cleanup stack");
                goto unwind;
            }
            break;
        }

        case OP_TRY_END:
            /* The normal-path pop.  A UCLEAN_F_RUNNING marker belongs to
             * the walker, never to a TRY_END.  A mismatch means the
             * emitter and the walker disagree about the stack's shape,
             * which would leak an entry rather than announce itself. */
            if (s->ncleanup > 0
                && s->cleanup[s->ncleanup - 1].kind == (uint8_t)UCLEAN_TRY
                && (s->cleanup[s->ncleanup - 1].flags & UCLEAN_F_RUNNING) == 0) {
                s->ncleanup--;
            } else {
                UGC_ASSERT(0);
            }
            break;

        case OP_PUSH_TAG: {
            /* A[3:0] = the register holding the tag, A[7:4] = flags, Bx =
             * the onleave handler.  A register that does not hold a tag
             * (and the explicit UCLEAN_F_FRESH_TAG request) opens a fresh
             * anonymous one, so `mytag: { ... }` scopes whether or not
             * `mytag` names a Tag. */
            uint8_t flags = (uint8_t)(OPA(i) >> 4);
            UTag *t = NULL;
            if ((flags & UCLEAN_F_FRESH_TAG) == 0) {
                UValue tv = R[OPA(i) & 0xFu];
                if (tv.kind == UV_CELL && ((UCell *)tv.v.p)->type == UCELL_TAG)
                    t = (UTag *)tv.v.p;
            }
            if (t == NULL) {
                t = utag_new(vm, uv_nil());
                if (t == NULL) {
                    (void)uexec_throw(vm, s, UP_OOMERROR, "tag push: out of memory creating the scope tag");
                    goto unwind;
                }
                f = &s->frames[s->nframes - 1];   /* the allocation may have collected */
            }
            UCleanup c;
            c.kind = (uint8_t)UCLEAN_TAG_SCOPE;
            c.flags = flags;
            c.saved_unwind = (uint8_t)UUNWIND_NONE;
            c.frame = (uint16_t)(s->nframes - 1);
            /* handler_pc names a TRY's catch or finally entry and means
             * nothing for a tag scope; the onleave body is the only
             * target this entry has. */
            c.handler_pc = 0;
            c.onleave_pc = OPBX(i);
            c.tag = t;
            c.saved = s->tag ? uv_ptr(UV_CELL, s->tag) : uv_nil();
            if (ustrand_push_cleanup(s, c) != 0) {
                (void)uexec_throw(vm, s, UP_OOMERROR, "tag push: out of memory growing the cleanup stack");
                goto unwind;
            }
            s->tag = t;
            utag_fire(vm, t->enter);
            break;
        }

        case OP_POP_TAG:
            if (s->ncleanup > 0 && s->cleanup[s->ncleanup - 1].kind == (uint8_t)UCLEAN_TAG_SCOPE) {
                UCleanup c = s->cleanup[--s->ncleanup];
                s->tag = (c.saved.kind == UV_CELL) ? (UTag *)c.saved.v.p : NULL;
                if (c.tag) utag_fire(vm, c.tag->leave);
            } else {
                UGC_ASSERT(0);   /* see OP_TRY_END */
            }
            break;

        case OP_LOAD_CATCH_VALUE:
            /* The handler owns the value from here.  Clearing the strand's
             * copy drops the last root the walker held on it, so an
             * exception the handler discards is collectable at once. */
            R[OPA(i)] = s->transfer;
            s->transfer = uv_nil();
            break;

        case OP_RESUME: {
            /* The end of a finally body the walker started.  Restore the
             * unwind it suspended and hand control back to the walk.  The
             * normal-path copy of a finally is jumped past, never resumed,
             * so an unmatched RESUME means a malformed chunk. */
            if (s->ncleanup == 0 || (s->cleanup[s->ncleanup - 1].flags & UCLEAN_F_RUNNING) == 0) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "unwind resume: no cleanup body in progress");
                goto unwind;
            }
            UCleanup mark = s->cleanup[--s->ncleanup];
            s->unwind = mark.saved_unwind;
            s->transfer = mark.saved;
            goto unwind;
        }

        default:
            (void)uexec_throw(vm, s, UP_TYPEERROR, "opcode not available in this build");
            goto unwind;
        }
        continue;
    unwind:
        if (uexec_unwind(vm, s) != 0) return s->state;
    }
}

/* The strand in dispatch is published on the scheduler for the duration
 * of the run: urbi_throw reads it to find where to deposit a native's
 * exception, and mark_fixed walks it.  Saved and restored so a native
 * that calls back into script (uexec_call) leaves the outer strand in
 * place when it returns. */
int uexec_run(UVM *vm, UStrand *s, uint32_t budget)
{
    UStrand *prev = vm->sched.current;
    vm->sched.current = s;
    int st = uexec_run_inner(vm, s, budget);
    vm->sched.current = prev;
    return st;
}

/* --- synchronous entry points -------------------------------------------- */

int uexec_call(UVM *vm, UStrand *s, UClosure *cl, UValue recv, const UValue *argv, uint8_t argc, UValue *out)
{
    if (cl == NULL) return uexec_throw(vm, s, UP_TYPEERROR, "call: callee is not a closure");

    if (cl->native) {
        if (argc < cl->min_args || argc > cl->max_args)
            return arity_error(vm, s, cl->name, cl->min_args, cl->max_args, argc);
        UValue res = uv_nil(), self = recv;
        USTRAND_ROOT(s, res); USTRAND_ROOT(s, self);
        int rc = cl->native(vm, self, (UValue *)argv, argc, &res);
        /* Unrooting in reverse order writes s->croots twice in a row and
         * the first write is dead, but the pairing is the macro protocol. */
        /* cppcheck-suppress redundantAssignment */
        USTRAND_UNROOT(s, self); USTRAND_UNROOT(s, res);
        if (rc == UEXEC_OK && out) *out = res;
        return rc;
    }

    UProto *p = cl->proto;
    if (p->arity_prologue ? (argc > p->nparams) : (argc != p->nparams))
        return arity_error_proto(vm, s, cl, argc);

    uint32_t base = 0;
    if (s->nframes > 0) {
        UFrame *top = &s->frames[s->nframes - 1];
        base = top->base + uproto_max_reg(top->closure ? top->closure->proto : NULL) + 1;
    }
    if (ustrand_ensure_stack(s, base + (uint32_t)p->max_reg + 1u) != 0)
        return uexec_throw(vm, s, UP_OOMERROR, "call: out of memory growing the register stack");
    for (uint8_t k = 0; k < argc; k++) s->stack[base + k] = argv[k];

    UValue clv = uv_ptr(UV_CELL, cl);
    USTRAND_ROOT(s, clv);
    int prc = ustrand_push_frame_args(s, cl, recv, base, 0, argc);
    USTRAND_UNROOT(s, clv);
    if (prc != 0) return uexec_throw(vm, s, UP_OOMERROR, "call: out of memory pushing a call frame");
    if (p->arity_prologue && p->nparams > 0) s->stack[base + p->nparams] = uv_int((int64_t)argc);
    s->frames[s->nframes - 1].is_boundary = 1;

    uint8_t saved_state = s->state;
    int st = uexec_run(vm, s, 0);
    /* An unwind that reached this boundary frame without finding a handler
     * popped it and came back here with s->unwind still set, so the native
     * that called in returns UEXEC_THROW and ITS caller keeps unwinding. */
    if (s->unwind != UUNWIND_NONE) return UEXEC_THROW;
    if (st == USTRAND_DEAD) return UEXEC_THROW;
    s->state = saved_state;
    if (out) *out = s->result;
    return UEXEC_OK;
}

int uexec_run_source(UVM *vm, URealm *realm, const char *src, size_t n,
                     const char *name, UValue *out, char *err, size_t errcap)
{
    if (out) *out = uv_nil();
    if (!vm || !realm) return URBI_ERR_INVALID_ARG;

    UProto *root = NULL;
    int rc = ufront_compile(vm, src, n, name, &root, err, errcap);
    if (rc != URBI_OK) return rc;

    UStrand *s = uvm_spare_acquire(vm, realm);
    if (!s) { uchunk_destroy(root, NULL); return URBI_ERR_OOM; }

    UProtoCell *pc = uproto_bind(vm, root);   /* takes ownership of root either way */
    if (!pc) { uvm_spare_release(vm, s); return URBI_ERR_OOM; }

    /* Pin across the closure allocation: the chunk is not yet reachable
     * from any closure, frame or register, and uclosure_new may collect. */
    pc->cell.flags |= UCELL_F_PINNED;
    UClosure *cl = uclosure_new(vm, root, 0);
    pc->cell.flags &= (uint16_t)~UCELL_F_PINNED;
    if (!cl) { uvm_spare_release(vm, s); return URBI_ERR_OOM; }
    if (vm->protos[UP_CLOSURE]) cl->proto_obj = vm->protos[UP_CLOSURE];

    UValue res = uv_nil();
    int crc = uexec_call(vm, s, cl, uv_obj(realm->globals), NULL, 0, &res);
    uvm_spare_release(vm, s);
    int rc2 = uexec_finish_run(vm, crc);
    if (rc2 == URBI_OK && out) *out = res;
    return rc2;
}
