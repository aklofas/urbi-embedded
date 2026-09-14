/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/uexec_ops.c — the bytecode dispatch loop.
 *
 * Scope: the sequential subset of the 49-opcode set.  Concurrency
 * (YIELD, FORK_*, JOIN_WAIT), unwinding (TRY_*, PUSH_TAG, POP_TAG,
 * RESUME, LOAD_CATCH_VALUE) and the reactive installs land with their
 * own tasks; every opcode not handled here throws through `default:`
 * rather than silently doing nothing.
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

/* Resolves `name` on an object receiver and calls it with one argument.
 * Returns 1 when the slot did not exist (caller falls through to its own
 * error), 0 on a completed call, or UEXEC_THROW. */
static int object_binop(UVM *vm, UStrand *s, UValue lhs, UValue rhs, const char *name, UValue *out)
{
    if (lhs.kind != UV_OBJ) return 1;
    const USym *sym = usym_cstr(vm, name);
    if (!sym) return uexec_throw(vm, s, UP_OOMERROR, "out of memory interning an operator name");
    UObjSlotRef ref;
    if (!uobj_resolve(vm, (UObject *)lhs.v.p, sym, &ref)) return 1;
    UValue fn = uv_nil();
    int rc = slot_read(vm, s, &ref, lhs, &fn);
    if (rc != UEXEC_OK) return rc;
    if (fn.kind != UV_CELL || ((UCell *)fn.v.p)->type != UCELL_CLOSURE) return 1;
    return uexec_call(vm, s, (UClosure *)fn.v.p, lhs, &rhs, 1, out);
}

static const char *arith_op_name(uint8_t op)
{
    switch (op) {
    case OP_ADD: return "+";
    case OP_SUB: return "-";
    case OP_MUL: return "*";
    default:     return "/";
    }
}

static int arith_type_error(UVM *vm, UStrand *s, uint8_t op)
{
    char msg[96]; size_t at = 0;
    msg[at++] = '\'';
    const char *p = arith_op_name(op); while (*p) msg[at++] = *p++;
    msg[at++] = '\'';
    p = " operands must be numbers, strings, or an object with a '";
    while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
    p = arith_op_name(op); while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
    p = "' slot"; while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
    msg[at] = '\0';
    return uexec_throw(vm, s, UP_TYPEERROR, msg);
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
            if (c.v.i == 0) return uexec_throw(vm, s, UP_DIVBYZERO, "division by zero");
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
            if (y == 0.0) return uexec_throw(vm, s, UP_DIVBYZERO, "division by zero");
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
        int rc = object_binop(vm, s, b, c, arith_op_name(op), out);
        if (rc != 1) return rc;
    }
    return arith_type_error(vm, s, op);
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
    return uexec_throw(vm, s, UP_TYPEERROR,
                       op == OP_LT ? "'<' operands must be comparable"
                                   : "'<=' operands must be comparable");
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

static int arity_error(UVM *vm, UStrand *s)
{
    return uexec_throw(vm, s, UP_ARITYERROR, "function call: wrong argument count");
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
        if (nargs < callee->min_args || nargs > callee->max_args) return arity_error(vm, s);
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
    if (p->arity_prologue ? (nargs > p->nparams) : (nargs != p->nparams)) return arity_error(vm, s);

    uint32_t new_base = base + a + arg_off;
    UValue clv = callee_v;
    USTRAND_ROOT(s, clv);
    int prc = ustrand_push_frame_args(s, callee, self_value, new_base, a, nargs);
    USTRAND_UNROOT(s, clv);
    if (prc != 0) return uexec_throw(vm, s, UP_OOMERROR, "function call: out of memory pushing a call frame");
    if (p->arity_prologue && p->nparams > 0) s->stack[new_base + p->nparams] = uv_int((int64_t)nargs);
    return UEXEC_OK;
}

/* --- the dispatch loop --------------------------------------------------- */

static int uexec_run_inner(UVM *vm, UStrand *s, uint32_t budget)
{
    bool unbounded = (budget == 0);
    s->state = USTRAND_RUNNING;
    for (;;) {
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
            UValue v = R[OPB(i)];
            if (v.kind == UV_INT) {
                int64_t r;
                if (__builtin_sub_overflow((int64_t)0, v.v.i, &r)) R[OPA(i)] = uv_float(-(double)v.v.i);
                else R[OPA(i)] = uv_int(r);
            } else if (v.kind == UV_FLOAT) {
                R[OPA(i)] = uv_float(-v.v.f);
            } else {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "unary '-' operand must be a number");
                goto unwind;
            }
            break;
        }

        case OP_RET: {
            UValue rv = R[OPA(i)];
            ustrand_close_upvals(s, f->base);
            uint8_t boundary = f->is_boundary;
            uint8_t rr = f->ret_reg;
            uint32_t caller_base = s->nframes > 1 ? s->frames[s->nframes - 2].base : 0;
            ustrand_pop_frame(s);
            /* The boundary test comes first: uexec_call's frame is often
             * the only one on the strand, and returning from it means "the
             * synchronous call finished", not "the strand died". */
            if (boundary) { s->result = rv; return USTRAND_RUNNING; }
            if (s->nframes == 0) { s->result = rv; s->state = USTRAND_DEAD; return s->state; }
            s->stack[caller_base + rr] = rv;
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
            if (s->realm == NULL || s->realm->globals == NULL) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "global access: strand has no realm");
                goto unwind;
            }
            R[OPA(i)] = uv_obj(s->realm->globals);
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

        case OP_SETSLOT: {
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
        if (argc < cl->min_args || argc > cl->max_args) return arity_error(vm, s);
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
    if (p->arity_prologue ? (argc > p->nparams) : (argc != p->nparams)) return arity_error(vm, s);

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
    if (crc != UEXEC_OK) return URBI_ERR_UNCAUGHT_THROW;
    if (out) *out = res;
    return URBI_OK;
}
