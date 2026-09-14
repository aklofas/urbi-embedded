/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/uunwind.c — the cleanup-stack walker: one error channel.
 *
 * Every runtime failure is a throw: an exception object lands in
 * s->transfer with s->unwind = UUNWIND_THROW, and the cleanup stack is
 * the only mechanism that gets control anywhere else.  RETURN and STOP
 * travel the same stack, so a `return` out of a try runs its finally and
 * a stopped tag scope fires its leave before the frame goes.
 *
 * ---------------------------------------------------------------------
 * The three shapes the emitter actually produces
 * ---------------------------------------------------------------------
 *
 * Read off `build/host/urbi --dump-bytecode` at the time this was
 * written; uemit_unwind.c's emit_try_frame is the producer.  handler_pc
 * (TRY_BEGIN's Bx) is an ABSOLUTE instruction index into the frame's own
 * proto, and flags (TRY_BEGIN's A) carries exactly ONE of
 * UCLEAN_F_HAS_CATCH / UCLEAN_F_HAS_FINALLY -- never both, because the
 * catch+finally source form is emitted as two nested entries.
 *
 *   try { a } catch (var e) { b }
 *     0  LOADNIL     rd
 *     1  TRY_BEGIN   flags=HAS_CATCH  Bx=5
 *     2  <body> ; MOVE rd, body
 *     3  TRY_END                       <- normal path pops the entry
 *     4  JMP past_handler
 *     5  LOAD_CATCH_VALUE e            <- handler_pc
 *     .  [guard: <expr>; TEST; JMP rethrow]
 *     .  <catch body> ; MOVE rd, catch
 *     .  [guard: JMP past; rethrow: THROW e; past:]
 *     6  past_handler:
 *
 *   try { a } finally { c }
 *     0  LOADNIL     rd
 *     1  TRY_BEGIN   flags=HAS_FINALLY Bx=6
 *     2  <body> ; MOVE rd, body
 *     3  TRY_END
 *     4  <finally body>                <- inline copy, normal path only
 *     5  JMP past_finally
 *     6  <finally body>                <- handler_pc: the unwind copy
 *     7  RESUME                        <- hands control back to the walker
 *     8  past_finally:
 *
 *   try { a } catch (var e) { b } finally { c }
 *     0  LOADNIL     rd
 *     1  TRY_BEGIN   flags=HAS_FINALLY Bx=<finally handler>   (outer)
 *     2  TRY_BEGIN   flags=HAS_CATCH   Bx=<catch handler>     (inner)
 *     .  <body> ; TRY_END (pops the inner) ; [else body] ; JMP past_catch
 *     .  catch handler: LOAD_CATCH_VALUE ... ; MOVE rd, catch
 *     .  past_catch: TRY_END (pops the outer)
 *     .  <finally inline copy> ; JMP past_finally
 *     .  finally handler: <finally unwind copy> ; RESUME
 *     .  past_finally:
 *
 * So a catch handler reaches its finally by falling through to the outer
 * TRY_END and the inline copy -- the walker never chains the two itself.
 * A throw raised INSIDE a catch body finds the outer HAS_FINALLY entry
 * still on the stack, which is what makes that case work.
 *
 * break/continue out of a try need nothing here: the emitter's
 * urbi_emit_scope_crossings plants TRY_END plus its own inline finally
 * copy (and OP_POP_TAG for a tag scope) ahead of the jump.  `return`
 * does NOT -- it emits a bare OP_RET -- so OP_RET routes through this
 * walker whenever the current frame still owns cleanup entries.
 *
 * ---------------------------------------------------------------------
 * Running a finally
 * ---------------------------------------------------------------------
 *
 * The unwind copy of a finally body lives in the SAME proto and register
 * window as the code that raised, so it runs as an ordinary continuation
 * of the current frame rather than a nested VM invocation: the walker
 * pops the TRY entry, pushes a UCLEAN_F_RUNNING marker holding the
 * suspended (unwind, transfer) pair, clears the pending unwind, points
 * the frame's pc at handler_pc and returns 0.  OP_RESUME pops the marker,
 * restores the pair and re-enters the walker.  An unwind raised inside
 * the body reaches the walker with the marker on top, which discards the
 * suspended pair -- REVIVAL C-1 replace-on-raise, and the same rule that
 * makes `return` inside a finally override a pending throw. */

#include "rt/uexec.h"

/* --- small freestanding string helpers -------------------------------- */

static size_t uw_append(char *buf, size_t cap, size_t at, const char *s)
{
    if (cap == 0) return 0;
    while (s && *s && at + 1 < cap) buf[at++] = *s++;
    buf[at] = '\0';
    return at;
}

/* --- pc -> source line ------------------------------------------------- */

/* Sums proto->line_deltas up to `pc`, taking an absolute checkpoint from
 * abs_lines whenever the delta byte is the INT8_MIN sentinel (the shape
 * uemit.c writes).  0 means "no position": either the proto carries no
 * line table or the pc is out of range. */
uint32_t uproto_line_at(const UProto *p, uint32_t pc)
{
    if (p == NULL || p->line_deltas == NULL) return 0;
    if ((size_t)pc >= p->instr_count) return 0;
    uint32_t line = 0;
    size_t abs_idx = 0;
    for (size_t i = 0; i <= (size_t)pc; i++) {
        int8_t d = p->line_deltas[i];
        if (d == (int8_t)(-128)) {
            while (abs_idx < p->abs_line_count && p->abs_lines[abs_idx].pc < i) abs_idx++;
            if (abs_idx < p->abs_line_count && p->abs_lines[abs_idx].pc == i)
                line = p->abs_lines[abs_idx++].line;
        } else {
            line = (uint32_t)((int32_t)line + (int32_t)d);
        }
    }
    return line;
}

static size_t uw_append_u32(char *buf, size_t cap, size_t at, uint32_t n)
{
    char tmp[12];
    size_t k = 0;
    do { tmp[k++] = (char)('0' + (n % 10u)); n /= 10u; } while (n);
    while (k && at + 1 < cap) buf[at++] = tmp[--k];
    buf[at] = '\0';
    return at;
}

/* "line N: " for an unnamed chunk, "<name>:N: " for a named one, and
 * nothing at all when the position is unknown.  The REPL compiles with a
 * NULL source name, which is what makes the corpus's `line 1: ` prefix
 * the common shape. */
size_t uexec_position_prefix(UStrand *s, char *buf, size_t cap, size_t at)
{
    if (s == NULL || s->nframes == 0) return at;
    const UFrame *f = &s->frames[s->nframes - 1];
    const UProto *p = (f->closure ? f->closure->proto : NULL);
    if (p == NULL || f->pc == NULL || p->instructions == NULL) return at;
    /* f->pc has already been advanced past the faulting instruction. */
    size_t off = (size_t)(f->pc - p->instructions);
    uint32_t line = uproto_line_at(p, (uint32_t)(off ? off - 1 : 0));
    if (line == 0) return at;
    const char *name = uproto_source_name(p);
    if (name != NULL && name[0] != '\0') {
        at = uw_append(buf, cap, at, name);
        at = uw_append(buf, cap, at, ":");
    } else {
        at = uw_append(buf, cap, at, "line ");
    }
    at = uw_append_u32(buf, cap, at, line);
    return uw_append(buf, cap, at, ": ");
}

/* --- reporting what escaped --------------------------------------------
 *
 * The old core's contract, pinned by control_transfer/throw_uncaught.chk
 * and exceptions/exceptions.chk: a scalar or string throw that escapes
 * everything recovers to nil and reports URBI_OK, while an exception
 * OBJECT surfaces as URBI_ERR_UNCAUGHT_THROW carrying its `message`.
 * vm->last_error is the signal -- empty means "recover to nil" -- so
 * this deliberately CLEARS the text uexec_throw wrote eagerly when the
 * escaped value is not an object. */
static void uexec_report_escape(UVM *vm, const UStrand *s)
{
    vm->last_error[0] = '\0';
    vm->last_error_code = URBI_OK;
    if (s->unwind != UUNWIND_THROW) return;

    UValue v = s->transfer;
    if (v.kind != UV_OBJ) return;

    vm->last_error_code = URBI_ERR_UNCAUGHT_THROW;
    UObject *o = (UObject *)v.v.p;
    const USym *kmsg = usym_cstr(vm, "message");
    UObjSlotRef ref;
    if (kmsg && o && uobj_resolve(vm, o, kmsg, &ref)) {
        UValue mv = uobj_slot_value(&ref);
        if (mv.kind == UV_SYM || mv.kind == UV_STR) {
            uint32_t len;
            const char *b = uv_str_bytes(mv, &len);
            (void)uw_append(vm->last_error, sizeof vm->last_error, 0, b);
            return;
        }
    }
    (void)uw_append(vm->last_error, sizeof vm->last_error, 0, "<exception>");
}

/* --- completing a return ------------------------------------------------ */

int uexec_return(UVM *vm, UStrand *s, UValue rv)
{
    (void)vm;
    if (s->nframes == 0) { s->result = rv; s->state = USTRAND_DEAD; return 1; }
    UFrame *f = &s->frames[s->nframes - 1];
    uint8_t  boundary = f->is_boundary;
    uint8_t  rr = f->ret_reg;
    uint32_t caller_base = s->nframes > 1 ? s->frames[s->nframes - 2].base : 0;
    ustrand_pop_frame(s);
    /* The boundary test comes first: uexec_call's frame is often the only
     * one on the strand, and returning from it means "the synchronous call
     * finished", not "the strand died". */
    if (boundary) { s->result = rv; s->state = USTRAND_RUNNING; return 1; }
    if (s->nframes == 0) { s->result = rv; s->state = USTRAND_DEAD; return 1; }
    s->stack[caller_base + rr] = rv;
    return 0;
}

/* --- the walker ---------------------------------------------------------- */

int uexec_unwind(UVM *vm, UStrand *s)
{
    for (;;) {
        if (s->unwind == UUNWIND_NONE) return 0;   /* nothing pending */
        if (s->nframes == 0) {
            /* Everything is gone: report and die. */
            uexec_report_escape(vm, s);
            s->state = USTRAND_DEAD;
            ustrand_close_upvals(s, 0);
            return 1;
        }

        uint16_t fi = (uint16_t)(s->nframes - 1);
        if (s->ncleanup > 0 && s->cleanup[s->ncleanup - 1].frame >= fi) {
            UCleanup top = s->cleanup[--s->ncleanup];

            if (top.kind == UCLEAN_TAG_SCOPE) {
                /* The tag object, its leave event and the STOP match land
                 * with the scheduler task; popping the entry is the whole
                 * of the scope's teardown until then. */
                continue;
            }

            if ((top.flags & UCLEAN_F_RUNNING) != 0) {
                /* An unwind raised inside a finally body replaces the one
                 * that body suspended (REVIVAL C-1). */
                continue;
            }

            /* The emitter never sets both bits on one entry (see the
             * layout note above); if it ever did, the finally would have
             * to run before the catch could be considered, and the entry
             * would have to survive the finally to be reconsidered. */
            UGC_ASSERT((top.flags & (UCLEAN_F_HAS_CATCH | UCLEAN_F_HAS_FINALLY))
                       != (UCLEAN_F_HAS_CATCH | UCLEAN_F_HAS_FINALLY));

            if ((top.flags & UCLEAN_F_HAS_FINALLY) != 0) {
                UCleanup mark = top;
                mark.flags = (uint8_t)(top.flags | UCLEAN_F_RUNNING);
                mark.saved_unwind = s->unwind;
                mark.saved = s->transfer;
                if (ustrand_push_cleanup(s, mark) != 0) {
                    /* No room to remember the suspended unwind, so the
                     * body cannot be run without losing it: skip the
                     * finally and keep unwinding rather than corrupt the
                     * walk.  Only reachable under allocation failure. */
                    continue;
                }
                s->unwind = UUNWIND_NONE;
                s->transfer = uv_nil();
                UFrame *f = &s->frames[fi];
                f->pc = f->closure->proto->instructions + top.handler_pc;
                return 0;
            }

            if (s->unwind == UUNWIND_THROW && (top.flags & UCLEAN_F_HAS_CATCH) != 0) {
                s->unwind = UUNWIND_NONE;
                UFrame *f = &s->frames[fi];
                f->pc = f->closure->proto->instructions + top.handler_pc;
                /* s->transfer stays put: OP_LOAD_CATCH_VALUE reads it. */
                return 0;
            }
            continue;
        }

        /* No handler left in this frame. */
        if (s->unwind == UUNWIND_RETURN) {
            UValue rv = s->transfer;
            s->unwind = UUNWIND_NONE;
            s->transfer = uv_nil();
            return uexec_return(vm, s, rv);
        }

        /* THROW (and, once the scheduler task lands, STOP) leaves the
         * frame behind and keeps looking.  Crossing a boundary frame
         * hands control back to the native that called in, with the
         * unwind still pending so ITS caller carries on unwinding. */
        {
            UFrame *f = &s->frames[fi];
            uint8_t boundary = f->is_boundary;
            ustrand_pop_frame(s);
            if (boundary) {
                uexec_report_escape(vm, s);
                s->state = USTRAND_RUNNING;
                return 1;
            }
        }
    }
}
