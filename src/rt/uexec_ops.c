/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/uexec_ops.c — the bytecode dispatch loop.
 *
 * Scope: the sequential subset of the 50-opcode set plus the cleanup
 * stack (TRY_BEGIN, TRY_END, THROW, RESUME, LOAD_CATCH_VALUE, PUSH_TAG,
 * POP_TAG), whose walker lives in rt/uunwind.c.  Concurrency (FORK_*,
 * JOIN_WAIT, TAG_STOP) and the reactive installs land with their own
 * tasks; every opcode not handled here throws through the unknown-opcode
 * arm rather than silently doing nothing.
 *
 * The tag scope OP_PUSH_TAG opens is a cleanup entry and nothing more
 * until the scheduler task gives it a tag object, an onleave body and a
 * stop to match -- enough for a try nested inside a tagged block to
 * unwind correctly, which is what the unwinder needs from it.
 *
 * Register addressing: f, R and K are held in locals across
 * instructions.  R is recomputed from s->stack + f->base after anything
 * that can grow the stack, and f is re-fetched from
 * s->frames[s->nframes - 1] for the same reason (both arrays are
 * reallocated in place by ustrand_ensure_stack / the frame grower); an
 * arm that may have done either ends in NEXT_RELOAD. */

#include "rt/uexec.h"
#include "rt/uslotcache.h"
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

static int slot_get(UVM *vm, UStrand *s, UValue recv, const USym *name, const char *what,
                    USlotCache *e, UValue *out)
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
    bool found = (e != NULL) ? uobj_resolve_flagging(vm, o, name, &ref)
                             : uobj_resolve(vm, o, name, &ref);
    if (!found) {
        /* A miss is a read too: the condition `at (Realm.x > 5)` installed
         * before anything declared `x` has to notice the declaration. */
        uwatch_observe(vm, o);
        char msg[192]; size_t at = 0;
        const char *p = what; while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
        p = ": slot '"; while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
        p = name->bytes; while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
        /* "Not found" and "gave up looking" are different answers, and a
         * proto graph wide or deep enough to exhaust the walk stack gets
         * the second one -- said out loud rather than mis-reported as a
         * missing slot. */
        p = uobj_resolve_overflowed(vm) ? "' unreachable: proto graph exceeds the 64-entry resolution stack"
                                        : "' not found";
        while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
        msg[at] = '\0';
        return uexec_throw(vm, s, UP_LOOKUPERROR, msg);
    }
    /* BOTH ends of the resolution.  The write that matters may land on the
     * receiver (SETSLOT creates a local slot that shadows the proto's) or
     * on the owner the value actually came from. */
    uwatch_observe(vm, o);
    uwatch_observe(vm, ref.owner);
    /* Filled before the read: a getter that runs script and bumps the
     * epoch leaves an entry that simply fails its next check. */
    if (e != NULL) {
        if (ref.owner == o) uslotcache_fill_own(vm, e, o, (uint16_t)ref.index);
        else uslotcache_fill_inherited(vm, e, o, &ref);
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

/* See rt/uexec.h. */
void uexec_note_write(UVM *vm, UObject *o, const USym *name, UValue v, bool existed)
{
    if (o->cell.flags & UOBJ_F_WATCHED) uwatch_mark_dirty(vm, o);
    if (!existed) {
        /* The write DECLARED the slot.  Installing is not changing, so it
         * arms a pending `x.changed?` subscription rather than firing it. */
        if (o->cell.flags & UOBJ_F_CHANGE_EVENTS) uwatch_slot_installed(vm, o, name);
        return;
    }
    int idx = uobj_find_local(o, name);
    if (idx >= 0 && (o->attrs[idx] & USLOT_CHANGED_EVENT))
        uwatch_slot_changed(vm, o, name, v);
}

/* The fork opcodes take a closure thunk the emitter built for the arm.
 * A native closure has no bytecode to run on a strand of its own, so it
 * is rejected here rather than deep inside usched_spawn. */
static bool fork_closure(UValue v)
{
    if (v.kind != UV_CELL || ((const UCell *)v.v.p)->type != UCELL_CLOSURE) return false;
    return ((const UClosure *)v.v.p)->proto != NULL;
}

/* The closure in a watcher-install operand register, or NULL. */
static UClosure *install_closure(UValue v)
{
    if (v.kind != UV_CELL || ((UCell *)v.v.p)->type != UCELL_CLOSURE) return NULL;
    return (UClosure *)v.v.p;
}

/* An OPTIONAL install operand: the emitter writes 0xFF into B or C when
 * the source had no body / no onleave. */
static UClosure *install_operand(const UValue *R, uint8_t reg)
{
    return reg == 0xFFu ? NULL : install_closure(R[reg]);
}

/* See rt/uexec.h.  `resume_slot` is biased by one so that zero reads as
 * "nothing pending" on a freshly zeroed strand. */
void ustrand_want_payload(UStrand *s)
{
    if (s == NULL || s->nframes == 0) return;
    const UFrame *f = &s->frames[s->nframes - 1];
    if (f->closure == NULL || f->closure->proto == NULL || f->pc == NULL) return;
    if (f->pc <= f->closure->proto->instructions) return;
    uint32_t call = f->pc[-1];
    if ((call & 0xFFu) != (uint32_t)OP_CALL) return;
    s->resume_slot = f->base + OPA(call) + 1u;
}

/* --- the dispatch loop --------------------------------------------------- */

/* Two dispatch forms over ONE set of opcode bodies.  GCC and Clang get a
 * label table; everything else, and any build with URBI_VM_FORCE_SWITCH,
 * gets the switch.  The bodies are written against OPCASE / NEXT and do
 * not know which they are in. */
#if defined(__GNUC__) && !(defined(URBI_VM_FORCE_SWITCH) && URBI_VM_FORCE_SWITCH)
#define UEXEC_THREADED 1
#else
#define UEXEC_THREADED 0
#endif

#define RELOAD() do { \
        f = &s->frames[s->nframes - 1]; \
        R = s->stack + f->base; \
        K = f->closure->proto->constants; \
    } while (0)

#if UEXEC_THREADED
#define OPCASE(n) L_##n
#define FETCH() do { \
        i = *f->pc++; \
        op = i & 0xFFu; \
        goto *(op < (uint32_t)OP_MAX ? labels[op] : &&L_unknown); \
    } while (0)
#else
#define OPCASE(n) case OP_##n
#define FETCH() goto fetch
#endif

/* NEXT: nothing the body did can have moved the frame or the stack.
 * NEXT_RELOAD: it may have.  When in doubt, NEXT_RELOAD. */
#define NEXT() FETCH()
#define NEXT_RELOAD() do { if (s->nframes == 0) goto no_frames; RELOAD(); FETCH(); } while (0)

#if UEXEC_THREADED
/* Labels as values are a GNU extension. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
static int uexec_run_inner(UVM *vm, UStrand *s, uint32_t budget)
{
#if UEXEC_THREADED
    static const void *const labels[OP_MAX] = {
#define URBI_OP(n, u, s) &&L_##n,
#include "chunk/uopcodes.def"
#undef URBI_OP
    };
#endif
    bool unbounded = (budget == 0);
    s->state = USTRAND_RUNNING;
    /* The safepoint a cross-strand stop is consumed at.  utag_stop only
     * marks (`unwind = STOP`, the tag in `transfer`) and wakes; the
     * marked strand walks its own cleanup stack here, the first time the
     * scheduler hands it back to dispatch.  Stop never runs on another
     * strand's stack. */
    if (s->unwind != UUNWIND_NONE) {
        /* An unwind owns `transfer`; a pending payload delivery does not
         * get to overwrite it. */
        s->resume_slot = 0;
        if (uexec_unwind(vm, s) != 0) return s->state;
    } else if (s->resume_slot != 0) {
        /* A native parked asking for the next wake's payload (see
         * ustrand_want_payload).  Deliver it now, before the instruction
         * after that OP_CALL runs. */
        uint32_t slot = s->resume_slot - 1u;
        s->resume_slot = 0;
        if (slot < s->stack_cap) s->stack[slot] = s->transfer;
        s->transfer = uv_nil();
    }
    /* The frame, its registers and its constants live in locals between
     * instructions.  They are re-derived (NEXT_RELOAD) after any arm that
     * can push or pop a frame or grow the register stack, which
     * reallocates it; an arm that provably cannot ends with NEXT. */
    UFrame *f;
    UValue *R;
    const UValue *K;
    uint32_t i, op;
    if (s->nframes == 0) goto no_frames;
    RELOAD();
#if UEXEC_THREADED
    FETCH();
#else
fetch:
    i = *f->pc++;
    op = i & 0xFFu;
    switch (op) {
#endif

        OPCASE(LOADK):    R[OPA(i)] = K[OPBX(i)]; NEXT();
        OPCASE(MOVE):     R[OPA(i)] = R[OPB(i)]; NEXT();
        OPCASE(LOADNIL):  R[OPA(i)] = uv_nil(); NEXT();
        OPCASE(LOADVOID): R[OPA(i)] = uv_void(); NEXT();
        OPCASE(LOADBOOL):
            R[OPA(i)] = uv_bool(OPB(i) != 0);
            if (OPC(i)) f->pc++;
            NEXT();

        OPCASE(ADD): OPCASE(SUB): OPCASE(MUL): OPCASE(DIV): {
            UValue res = uv_nil();
            if (arith(vm, s, (uint8_t)op, R[OPB(i)], R[OPC(i)], &res) != UEXEC_OK) goto unwind;
            f = &s->frames[s->nframes - 1];
            s->stack[f->base + OPA(i)] = res;
            NEXT_RELOAD();
        }

        OPCASE(NEG): {
            UValue res = uv_nil();
            if (negate(vm, s, R[OPB(i)], &res) != UEXEC_OK) goto unwind;
            f = &s->frames[s->nframes - 1];
            s->stack[f->base + OPA(i)] = res;
            NEXT_RELOAD();
        }

        OPCASE(RET): {
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
            NEXT_RELOAD();
        }

        OPCASE(YIELD):
            /* `;` is a sequence point, and a sequence point is where the
             * reactive runtime gets to look: an `at sync` body has to have
             * run before the NEXT statement of this strand, which is what
             * this drain buys and nothing else does.  A strand that may
             * not be descheduled is inside somebody's synchronous call --
             * a comparator, a getter, a watcher body -- and drains
             * nothing, which is also what keeps a drain from nesting. */
            /* A scheduled strand goes READY at the queue tail.  A strand
             * that may not be descheduled -- a spare, or one inside a
             * synchronous uexec_call -- treats the yield as the plain
             * sequence point it also is: its caller is a C frame waiting
             * for a value, and returning here would hand back a stale
             * one.  That is what a `;` inside a sort comparator, a getter
             * or an operator overload compiles to. */
            if (!usched_may_deschedule(s)) NEXT();
            if (vm->watch.ndirty != 0) uwatch_drain(vm);
            /* No register refresh: the drain runs every condition and every
             * sync body on a SPARE strand and spawns the rest, so it never
             * grows THIS strand's stack. */
            /* Nobody to yield to and nothing pending against this strand:
             * the round trip through the scheduler would re-dispatch this
             * same strand, so skip it.  "Nothing pending" is everything the
             * scheduler would act on between the two runs -- a body the
             * drain spawned, a write the drain made (the scheduler drains
             * again before re-dispatching), a stop or a gate the drain
             * applied to this strand, an ISR injection waiting for the next
             * step.  The cap is what keeps a lone busy strand from starving
             * timers and the host pump: at most UEXEC_FAST_YIELD_CAP
             * statements late.  The scheduler zeroes the count each time it
             * dispatches the strand. */
            if (vm->sched.run_head == NULL
                && vm->watch.ndirty == 0
                && s->unwind == (uint8_t)UUNWIND_NONE
                && s->gates == 0
                && s->state == (uint8_t)USTRAND_RUNNING
                && __atomic_load_n(&vm->sched.isr_head, __ATOMIC_RELAXED) == vm->sched.isr_tail
                && s->fast_yields < UEXEC_FAST_YIELD_CAP) {
                s->fast_yields++;
                NEXT();
            }
            s->state = USTRAND_READY;
            return s->state;

        OPCASE(GETUPVAL): {
            UClosure *cl = f->closure;
            if (OPB(i) >= cl->nupvals || cl->upvals[OPB(i)] == NULL) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "upvalue read: index out of range");
                goto unwind;
            }
            R[OPA(i)] = *cl->upvals[OPB(i)]->ptr;
            NEXT();
        }
        OPCASE(SETUPVAL): {
            UClosure *cl = f->closure;
            if (OPB(i) >= cl->nupvals || cl->upvals[OPB(i)] == NULL) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "upvalue write: index out of range");
                goto unwind;
            }
            *cl->upvals[OPB(i)]->ptr = R[OPA(i)];
            NEXT();
        }

        OPCASE(CLOSURE): {
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
            NEXT_RELOAD();
        }

        OPCASE(CLOSE): ustrand_close_upvals(s, f->base + OPA(i)); NEXT_RELOAD();

        OPCASE(CALL): {
            if (do_call(vm, s, (uint16_t)(s->nframes - 1), i) != UEXEC_OK) goto unwind;
            if (s->state != USTRAND_RUNNING) return s->state;
            NEXT_RELOAD();
        }

        OPCASE(JMP): {
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
            NEXT();
        }

        OPCASE(TEST):
            if ((int)uv_truthy(R[OPA(i)]) == (int)OPC(i)) f->pc++;
            NEXT();
        OPCASE(TESTSET):
            if ((int)uv_truthy(R[OPB(i)]) == (int)OPC(i)) f->pc++;
            else R[OPA(i)] = R[OPB(i)];
            NEXT();

        OPCASE(EQ): {
            bool eq = false;
            if (compare_eq(vm, s, R[OPB(i)], R[OPC(i)], &eq) != UEXEC_OK) goto unwind;
            f = &s->frames[s->nframes - 1];
            if ((int)eq != (int)OPA(i)) f->pc++;
            NEXT_RELOAD();
        }
        OPCASE(LT): OPCASE(LE): {
            bool r = false;
            if (compare_lt_le(vm, s, (uint8_t)op, R[OPB(i)], R[OPC(i)], &r) != UEXEC_OK) goto unwind;
            f = &s->frames[s->nframes - 1];
            if ((int)r != (int)OPA(i)) f->pc++;
            NEXT_RELOAD();
        }

        OPCASE(LOAD_REALM_GLOBAL): {
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
            uwatch_observe(vm, g);
            R[OPA(i)] = uv_obj(g);
            NEXT();
        }
        OPCASE(LOAD_RECV): R[OPA(i)] = f->recv; NEXT();

        OPCASE(GETSLOT): OPCASE(SELF): {
            USym **names = uproto_names(f->closure->proto);
            if (names == NULL || OPC(i) >= f->closure->proto->ic_count) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "slot access: no name table bound");
                goto unwind;
            }
            const USym *name = names[OPC(i)];
            UValue recv = R[OPB(i)];
            UObject *o = (recv.kind == UV_OBJ) ? (UObject *)recv.v.p : uv_dispatch_proto(vm, recv);
            USlotCache *e = o ? uslotcache_site(vm, f->closure->proto, (uint16_t)OPC(i)) : NULL;
            UValue out = uv_nil();
            if (e != NULL && uslotcache_hit(&vm->objstats, e, o, name)
                && (e->owner->attrs[e->index] & (USLOT_GETTER | USLOT_SETTER)) == 0) {
                USLOTCACHE_VERIFY(vm, o, name, e);
                vm->objstats.cache_hits++;
                uwatch_observe(vm, o);
                uwatch_observe(vm, e->owner);
                out = e->owner->values[e->index];
                if (op == OP_SELF) R[OPA(i) + 1u] = recv;   /* receiver first — dst may alias recv */
                R[OPA(i)] = out;
                NEXT();
            }
            const char *what = (op == OP_SELF) ? "method call" : "slot access";
            if (slot_get(vm, s, recv, name, what, e, &out) != UEXEC_OK) goto unwind;
            f = &s->frames[s->nframes - 1];
            R = s->stack + f->base;
            if (op == OP_SELF) R[OPA(i) + 1u] = recv;
            R[OPA(i)] = out;
            NEXT_RELOAD();
        }

        OPCASE(SETSLOT): OPCASE(SETSLOT_UPDATE): {
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
            USym *name = names[OPC(i)];
            USlotCache *e = uslotcache_site(vm, f->closure->proto, (uint16_t)OPC(i));
            /* An own plain slot on a writable receiver: every check the
             * long path makes below is one of these four, read live. */
            if (e != NULL && e->owner == o && uslotcache_hit(&vm->objstats, e, o, name)
                && (o->attrs[e->index] & (USLOT_CONSTANT | USLOT_GETTER | USLOT_SETTER)) == 0
                && (o->cell.flags & UOBJ_F_READONLY) == 0) {
                USLOTCACHE_VERIFY(vm, o, name, e);
                vm->objstats.cache_hits++;
                UValue written = R[OPA(i)];
                o->values[e->index] = written;
                uexec_note_write(vm, o, name, written, true);
                NEXT_RELOAD();
            }
            /* UPDATE is the bare-name write, `x = 1`.  It rebinds an
             * existing name and never declares one, so a name that
             * resolves nowhere on the chain is a LookupError rather than
             * a silent new global -- which is what makes a typo in an
             * assignment reportable.  `var x = 1` and the explicit
             * `Realm.x = 1` both emit plain SETSLOT and still create. */
            if (op == OP_SETSLOT_UPDATE) {
                UObjSlotRef probe;
                if (!uobj_resolve(vm, o, names[OPC(i)], &probe)) {
                    char msg[192]; size_t at = 0;
                    const char *p = "slot write: slot '";
                    while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
                    p = names[OPC(i)]->bytes;
                    while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
                    p = uobj_resolve_overflowed(vm)
                        ? "' unreachable: proto graph exceeds the 64-entry resolution stack"
                        : "' not found";
                    while (*p && at + 1 < sizeof msg) msg[at++] = *p++;
                    msg[at] = '\0';
                    (void)uexec_throw(vm, s, UP_LOOKUPERROR, msg);
                    goto unwind;
                }
                /* CONSTNESS IS INHERITED BY AN UPDATE, and only by one.
                 * `Object = 42` names the binding the built-in globals
                 * hold and asks to change it, so a constant anywhere on
                 * the chain refuses -- on the walk that has just proved
                 * the name resolves at all, so it costs nothing extra.
                 * A CREATE is a different request: `var Object = 42`,
                 * `class Pair { ... }` and `Realm.Object = 42` add a slot
                 * of their own that SHADOWS the built-in for one object,
                 * which is ordinary prototype shadowing and stays legal.
                 * The three are indistinguishable at this opcode anyway --
                 * same receiver, same instruction -- so a create-side rule
                 * could not tell them apart even if one were wanted. */
                if (uobj_slot_attrs(&probe) & USLOT_CONSTANT) {
                    (void)uexec_throw(vm, s, UP_TYPEERROR, "slot write: slot is constant");
                    goto unwind;
                }
            }
            if (o->cell.flags & UOBJ_F_READONLY) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "slot write: receiver is read-only");
                goto unwind;
            }
            int idx = uobj_find_local(o, name);
            if (idx >= 0 && (o->attrs[idx] & USLOT_CONSTANT)) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "slot write: slot is constant");
                goto unwind;
            }
            UValue written = R[OPA(i)];
            if (idx >= 0 && (o->attrs[idx] & (USLOT_GETTER | USLOT_SETTER))) {
                UProps *pr = (UProps *)o->values[idx].v.p;
                if ((o->attrs[idx] & USLOT_SETTER) && pr->setter.kind == UV_CELL) {
                    UValue arg = written, ignored = uv_nil();
                    if (uexec_call(vm, s, (UClosure *)pr->setter.v.p, recv, &arg, 1, &ignored) != UEXEC_OK) goto unwind;
                } else {
                    pr->value = written;
                }
                uexec_note_write(vm, o, name, written, true);
                NEXT_RELOAD();
            }
            int at = uobj_set_local(vm, o, name, written, 0);
            if (at < 0) {
                (void)uexec_throw(vm, s, UP_OOMERROR, "slot write: out of memory");
                goto unwind;
            }
            if (e != NULL) uslotcache_fill_own(vm, e, o, (uint16_t)at);
            uexec_note_write(vm, o, name, written, idx >= 0);
            NEXT_RELOAD();
        }

        OPCASE(THROW):
            (void)uexec_throw_value(vm, s, R[OPA(i)]);
            goto unwind;

        /* --- concurrency ------------------------------------------------
         *
         * All three spawn through usched_spawn, the one spawn path: the
         * child lands in the parent's realm under the parent's ambient
         * tag, with the parent's receiver, so a forked arm resolves
         * `Realm.x` and `this` exactly as the code around it does. */

        OPCASE(FORK_DETACH): {
            UValue cv = R[OPA(i)];
            if (!fork_closure(cv)) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "',' (parallel fork): operand is not a closure");
                goto unwind;
            }
            if (usched_spawn(vm, s->realm, (UClosure *)cv.v.p, s->tag, f->recv, NULL, 0) == NULL) {
                (void)uexec_throw(vm, s, UP_OOMERROR, "',' (parallel fork): cannot spawn the child strand");
                goto unwind;
            }
            NEXT_RELOAD();
        }

        OPCASE(FORK_JOIN): {
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
            NEXT_RELOAD();
        }

        OPCASE(JOIN_WAIT): {
            UValue cv = R[OPA(i)];
            if (cv.kind != UV_CELL || ((UCell *)cv.v.p)->type != UCELL_STRAND) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "'&' (parallel join): join operand is not a strand");
                goto unwind;
            }
            UStrand *child = (UStrand *)cv.v.p;
            if (child->state == USTRAND_DEAD) NEXT_RELOAD();   /* already finished */
            if (usched_park(s, &child->joiners, 0) == 0) return s->state;
            /* No scheduler to hand control back to (a spare strand, or
             * inside a synchronous call): the join still has to wait, so
             * the child runs nested on this stack instead. */
            usched_run_inline(vm, child);
            NEXT_RELOAD();
        }

        /* --- the reactive installs --------------------------------------
         *
         * All six carry their operands the same way: A is the condition
         * closure or the event, B the body and C the onleave (or the
         * `else` body, which the emitter puts in the same register), with
         * 0xFF in B or C meaning "absent".  The watcher takes the
         * installing strand's realm and its ambient tag, so `mytag: at (c)
         * body` is cancelled by `mytag.stop()`. */

        OPCASE(AT_INSTALL): OPCASE(AT_SYNC_INSTALL): OPCASE(WHENEVER_INSTALL): {
            UClosure *cond = install_closure(R[OPA(i)]);
            if (cond == NULL) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "at watcher install: condition is not a closure");
                goto unwind;
            }
            uint8_t mode = (op == OP_AT_SYNC_INSTALL)  ? (uint8_t)UWATCH_AT_SYNC
                         : (op == OP_WHENEVER_INSTALL) ? (uint8_t)UWATCH_WHENEVER
                         :                                       (uint8_t)UWATCH_AT;
            if (uwatch_install(vm, s, mode, cond, NULL,
                               install_operand(R, OPB(i)), install_operand(R, OPC(i))) == NULL) {
                (void)uexec_throw(vm, s, UP_OOMERROR, "at watcher install: out of memory");
                goto unwind;
            }
            NEXT_RELOAD();
        }

        OPCASE(AT_EVENT_INSTALL): OPCASE(AT_EVENT_SYNC_INSTALL): OPCASE(WHENEVER_EVENT_INSTALL): {
            UValue ev = R[OPA(i)];
            if (ev.kind != UV_CELL || ((UCell *)ev.v.p)->type != UCELL_EVENT) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "at-event watcher install: operand is not an event");
                goto unwind;
            }
            /* An event subscription fires per emission, so `whenever (e?)`
             * and `at (e?)` are the same watcher; only the SYNC form
             * differs, by running its body inline under syncEmit. */
            uint8_t mode = (op == OP_AT_EVENT_SYNC_INSTALL)
                         ? (uint8_t)UWATCH_AT_SYNC : (uint8_t)UWATCH_AT;
            if (uwatch_install(vm, s, mode, NULL, (UEvent *)ev.v.p,
                               install_operand(R, OPB(i)), install_operand(R, OPC(i))) == NULL) {
                (void)uexec_throw(vm, s, UP_OOMERROR, "at-event watcher install: out of memory");
                goto unwind;
            }
            NEXT_RELOAD();
        }

        OPCASE(WAITUNTIL_INSTALL): {
            UClosure *cond = install_closure(R[OPA(i)]);
            if (cond == NULL) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "waituntil install: condition is not a closure");
                goto unwind;
            }
            int rc = uwatch_waituntil(vm, s, cond);
            if (rc < 0) goto unwind;              /* the condition raised, or OOM */
            if (rc > 0) return s->state;          /* parked until it holds */
            NEXT_RELOAD();                        /* already true: carry straight on */
        }

        OPCASE(GETSLOT_CHANGE_EVENT): {
            USym **names = uproto_names(f->closure->proto);
            if (names == NULL || OPC(i) >= f->closure->proto->ic_count) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "slot-change event: no name table bound");
                goto unwind;
            }
            UValue recv = R[OPB(i)];
            if (recv.kind != UV_OBJ) {
                (void)uexec_throw(vm, s, UP_TYPEERROR, "slot-change event: receiver is not an Object");
                goto unwind;
            }
            UEvent *e = uwatch_slot_change_event(vm, (UObject *)recv.v.p, names[OPC(i)]);
            if (e == NULL) {
                (void)uexec_throw(vm, s, UP_OOMERROR, "slot-change event: out of memory");
                goto unwind;
            }
            f = &s->frames[s->nframes - 1];       /* the lookup allocated */
            R = s->stack + f->base;
            R[OPA(i)] = uv_ptr(UV_CELL, e);
            NEXT_RELOAD();
        }

        /* --- the cleanup stack (see rt/uunwind.c for the layouts) ------ */

        OPCASE(TRY_BEGIN): {
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
            NEXT_RELOAD();
        }

        OPCASE(TRY_END):
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
            NEXT_RELOAD();

        OPCASE(PUSH_TAG): {
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
                /* The allocation may have moved the frame and register
                 * arrays.  Nothing below reads `f` or `R` -- the entry is
                 * built from s->nframes and the arm ends in NEXT_RELOAD --
                 * so there is nothing to refresh. */
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
            /* Newcomers are gated.  Entering the scope of a blocked or
             * frozen tag takes that tag's bits and parks here, so a
             * strand cannot slip into a stopped-short scope and run;
             * unblocking releases it with every other member.  A strand
             * that may not park (a spare, or one inside a call boundary)
             * keeps the bit and is stopped at its next enqueue. */
            {
                uint8_t g = utag_gate_bits(t);
                if (g != 0) {
                    s->gates = (uint8_t)(s->gates | g);
                    if (usched_park(s, NULL, 0) == 0) return s->state;
                }
            }
            NEXT_RELOAD();
        }

        OPCASE(POP_TAG):
            if (s->ncleanup > 0 && s->cleanup[s->ncleanup - 1].kind == (uint8_t)UCLEAN_TAG_SCOPE) {
                UCleanup c = s->cleanup[--s->ncleanup];
                s->tag = (c.saved.kind == UV_CELL) ? (UTag *)c.saved.v.p : NULL;
                if (c.tag) utag_fire(vm, c.tag->leave);
            } else {
                UGC_ASSERT(0);   /* see OP_TRY_END */
            }
            NEXT_RELOAD();

        OPCASE(LOAD_CATCH_VALUE):
            /* The handler owns the value from here.  Clearing the strand's
             * copy drops the last root the walker held on it, so an
             * exception the handler discards is collectable at once. */
            R[OPA(i)] = s->transfer;
            s->transfer = uv_nil();
            NEXT();

        OPCASE(RESUME): {
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

        /* In the wire set, not implemented here. */
        OPCASE(NEQ): OPCASE(TAG_STOP): OPCASE(PUSH_FRAME_GUARD):
            goto L_unknown_arm;

#if UEXEC_THREADED
    L_unknown:
#else
        default:
#endif
    L_unknown_arm:
            (void)uexec_throw(vm, s, UP_TYPEERROR, "opcode not available in this build");
            goto unwind;
#if !UEXEC_THREADED
    }
#endif

unwind:
    if (uexec_unwind(vm, s) != 0) return s->state;
    NEXT_RELOAD();

no_frames:
    /* Nothing to dispatch.  Reachable only if a caller enters with an
     * empty strand or the unwinder leaves one behind; both are bugs, so
     * trap in debug builds and die cleanly otherwise rather than indexing
     * frames[(uint16_t)-1]. */
    UGC_ASSERT(0);
    s->state = USTRAND_DEAD;
    return s->state;
}
#if UEXEC_THREADED
#pragma GCC diagnostic pop
#endif

#undef NEXT_RELOAD
#undef NEXT
#undef FETCH
#undef OPCASE
#undef RELOAD


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
    s->nboundary++;

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

/* --- running a whole chunk ------------------------------------------------
 *
 * A chunk runs on a SCHEDULED strand of its realm, under the realm's
 * connection tag -- not synchronously on a spare.  That is what makes the
 * concurrency separators mean the same thing at chunk top as they do
 * inside a function: `,` really detaches, `&` really joins, `sleep` and a
 * tag scope really park.
 *
 * The pump runs until nothing is READY.  It deliberately does NOT wait
 * for pending timers: a chunk that slept, or that armed an `every`, hands
 * back nil and resumes on a later urbi_step, which is exactly the
 * line-at-a-time model the REPL and the host driver need (and the only
 * one under which an `every` does not mean "never return"). */
int uexec_run_chunk(UVM *vm, URealm *realm, UClosure *cl, UValue *out)
{
    /* A realm whose globals are gone (urealm_free leaves it that way) is
     * not short-circuited here: the chunk runs and OP_LOAD_REALM_GLOBAL
     * throws, which is the one guard that also covers a realm freed while
     * its strands are mid-run. */
    UValue recv = realm->globals ? uv_obj(realm->globals) : uv_nil();
    /* usched_spawn allocates; the closure is reachable from nothing yet. */
    cl->cell.flags |= UCELL_F_RTPIN;
    UStrand *s = usched_spawn(vm, realm, cl, realm->root_tag, recv, NULL, 0);
    cl->cell.flags &= (uint16_t)~UCELL_F_RTPIN;
    if (!s) return URBI_ERR_OOM;

    /* Held across the pump: the step that sees the strand die unlinks it
     * from its realm, and then nothing else keeps the cell addressable. */
    s->cell.flags |= UCELL_F_RTPIN;
    /* Saved and restored rather than assigned, so a native that calls
     * back in through urbi_run leaves the outer chunk's claim in place. */
    USched *sc = uvm_sched(vm);
    UStrand *prev_awaited = sc->awaited;
    sc->awaited = s;
    while (usched_step(vm, 0, NULL) == USTEP_RAN) { }
    sc->awaited = prev_awaited;
    bool died = (s->state == USTRAND_DEAD);
    bool threw = died && s->unwind == (uint8_t)UUNWIND_THROW;
    UValue res = died ? s->result : uv_nil();
    /* vm->last_error is one buffer and this pump may have outlived the
     * strand: any detached strand that died after it left ITS message
     * there on the way to the diag hook, which is the channel a strand
     * nobody awaits reports through.  Render this strand's again, so the
     * message the caller reads is the failure it is being handed. */
    if (threw) uexec_report_escape(vm, s);
    s->cell.flags &= (uint16_t)~UCELL_F_RTPIN;

    int rc = uexec_finish_run(vm, threw ? UEXEC_THROW : UEXEC_OK);
    if (rc == URBI_OK && out) *out = res;
    return rc;
}

int uexec_run_source(UVM *vm, URealm *realm, const char *src, size_t n,
                     const char *name, UValue *out, char *err, size_t errcap)
{
    if (out) *out = uv_nil();
    if (!vm || !realm) return URBI_ERR_INVALID_ARG;

    UProto *root = NULL;
    int rc = ufront_compile(vm, src, n, name, &realm->budget, &root, err, errcap);
    if (rc != URBI_OK) return rc;

    UProtoCell *pc = uproto_bind(vm, root);   /* takes ownership of root either way */
    if (!pc) return URBI_ERR_OOM;

    /* Pin across the closure allocation: the chunk is not yet reachable
     * from any closure, frame or register, and uclosure_new may collect. */
    pc->cell.flags |= UCELL_F_RTPIN;
    UClosure *cl = uclosure_new(vm, root, 0);
    pc->cell.flags &= (uint16_t)~UCELL_F_RTPIN;
    if (!cl) return URBI_ERR_OOM;
    if (vm->protos[UP_CLOSURE]) cl->proto_obj = vm->protos[UP_CLOSURE];

    return uexec_run_chunk(vm, realm, cl, out);
}
