/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/ustrand.h — strands: the growable register stack, call frames,
 * open/closed upvalues, closures, and the cleanup stack for the
 * refound/core runtime.
 *
 * UProto comes from the kept frontend (src/chunk/uproto.h) rather than a
 * second rt-local definition -- the emitter that produces it is not being
 * rewritten, only the VM that runs it. ustrand.c reads only
 * proto->max_reg, proto->nparams, and proto->instructions. */

#ifndef URT_STRAND_H
#define URT_STRAND_H
#include "rt/ulist.h"
#include "chunk/uproto.h"

struct URealm; struct UTag;

#define UCLOSURE_ANY_ARGS 255

/* chunk/uproto.h already declares `typedef struct UClosure UClosure;`
 * (forward, opaque -- the old core completes it elsewhere); this completes
 * the same tag rather than re-typedef'ing the name, which pedantic C99
 * flags as a redefinition even when harmless. */
struct UClosure {
    UCell     cell;
    UProto   *proto;             /* NULL for a native closure */
    UObject  *proto_obj;         /* Closure prototype object for method dispatch; may be NULL */
    int      (*native)(struct UVM *, UValue self, UValue *args, uint8_t nargs, UValue *out);
    /* max_args == UCLOSURE_ANY_ARGS accepts any count at or above
     * min_args; the boot table spells the same value UMETHOD_VARARGS. */
    /* The name this closure was installed under, for diagnostics only.
     * NULL when it has none (a script function literal).  Never freed and
     * never traced: every writer stores either a boot-table string literal
     * or an interned USym's bytes, both immortal. */
    const char *name;
    uint8_t   min_args, max_args, nupvals;
    struct UUpval *upvals[1];    /* flexible array; nupvals entries */
};

typedef struct UUpval {
    UCell     cell;
    UValue   *ptr;               /* == stack + stack_index while open; == &closed once closed */
    UValue    closed;
    uint32_t  stack_index;
    struct UUpval *next_open;    /* open_upvals list, kept sorted by descending stack_index */
} UUpval;

typedef struct UFrame {
    UClosure *closure;
    const uint32_t *pc;
    uint32_t  base;
    uint8_t   ret_reg;
    uint8_t   is_boundary;       /* the synchronous "run until this frame returns" marker used by ustrand_call */
    UValue    recv;
} UFrame;

typedef enum { UCLEAN_TRY = 1, UCLEAN_TAG_SCOPE = 2 } UCleanKind;

/* UCLEAN_F_HAS_CATCH / _HAS_FINALLY are the bytecode's own flag bits:
 * OP_SCOPE_TRY carries them verbatim in A.  They must keep the values the
 * emitter writes (USCOPE_F_HAS_CATCH / USCOPE_F_HAS_FINALLY in
 * src/chunk/uchunk.h); this header sits below the chunk format in the
 * include order, so the values are restated rather than shared.
 *
 * UCLEAN_F_RUNNING is the unwinder's own and never appears in bytecode:
 * it marks the boundary entry the walker leaves behind while a finally
 * body runs, holding the unwind that body suspended. */
#define UCLEAN_F_HAS_CATCH   0x1u
#define UCLEAN_F_HAS_FINALLY 0x2u
#define UCLEAN_F_RUNNING     0x20u

typedef struct UCleanup {
    uint8_t   kind, flags;
    uint8_t   saved_unwind;      /* UCLEAN_F_RUNNING only: the suspended UUnwindKind */
    uint16_t  frame;             /* index into frames[] of the frame that pushed this */
    /* onleave_pc on a UCLEAN_F_RUNNING marker: with saved_unwind NONE
     * (a finally run by SCOPE_POP on the normal path), the instruction
     * index RESUME continues at; with saved_unwind JUMP, the entries the
     * suspended UNWIND_TO still has to pop. */
    uint32_t  handler_pc, onleave_pc;
    /* TAG_SCOPE: the tag this scope OPENED -- what a cross-strand STOP
     * matches against to find the scope it must unwind to. */
    struct UTag *tag;
    /* UCLEAN_F_RUNNING: the transfer value the finally body suspended.
     * TAG_SCOPE: the ambient tag this scope displaced, as a UV_CELL (nil
     * when the strand had none), restored into s->tag on the way out.
     * The two uses never meet -- a TAG_SCOPE entry is never a finally
     * marker. */
    UValue    saved;
} UCleanup;

/* Stored in UStrand.state (uint8_t) -- deliberately not a typedef'd
 * UStrandState: chunk/uproto.h drags in include/urbi/types.h, which
 * already binds that name to the public urbi_strand_state() enum (a
 * different, unrelated set of values). */
enum { USTRAND_READY = 0, USTRAND_RUNNING, USTRAND_PARKED, USTRAND_DEAD };
typedef enum { UUNWIND_NONE = 0, UUNWIND_RETURN, UUNWIND_THROW, UUNWIND_STOP,
               UUNWIND_JUMP, /* UNWIND_TO: pop jump_depth entries, then jump to the pc in transfer */
               UUNWIND_FATAL /* a chunk-integrity failure (uexec_fatal): a throw no catch takes and no finally runs for */
             } UUnwindKind;
/* A jump raised inside a finally body counts that body's UCLEAN_F_RUNNING
 * marker as one of its jump_depth entries, whether the walker or a
 * run-finally SCOPE_POP started the body. */
#define USTRAND_GATE_BLOCKED 0x1
#define USTRAND_GATE_FROZEN  0x2
/* How many yields in a row a lone strand may take without going back to
 * the scheduler: the bound on how late a timer that comes due meanwhile
 * can fire, counted in statements. */
#define UEXEC_FAST_YIELD_CAP 64

typedef struct UStrand {
    UCell      cell;
    struct UVM *vm; struct URealm *realm;
    /* Small, stable, monotonic -- what `Job.uid()` reports.  An address
     * would be neither reproducible across runs nor safe to hand to
     * script. */
    uint32_t   id;
    UValue    *stack; uint32_t stack_cap;
    UFrame    *frames; uint16_t nframes, frames_cap;
    uint16_t   nboundary;        /* how many frames[] entries have is_boundary set */
    uint8_t    fast_yields;      /* consecutive OP_YIELDs taken without a scheduler round trip */
    uint8_t    jump_depth;       /* UUNWIND_JUMP: scope entries still to pop (fills the byte before open_upvals) */
    UUpval    *open_upvals;
    struct UTag *tag;
    UCleanup  *cleanup; uint16_t ncleanup, cleanup_cap;
    uint8_t    state, gates, unwind, is_spare;
    UValue     transfer;
    struct UStrand *link;          /* run queue / wait list / timer heap linkage (heap uses an array; link unused there) */
    void      *waiting_on;
    uint64_t   wake_us;
    UValue     result;             /* value of the top frame's RET */
    struct UStrand *next_in_realm;
    struct UStrand *joiners;       /* strands parked on this one, threaded via their own `link` */
    /* One PLUS the absolute stack index the next wake's payload is
     * delivered to; 0 is "none", so a zeroed strand starts correct.
     *
     * A native that parks (`e.waituntil()`) has already had its nil result
     * written into the caller's destination register by the time the
     * strand stops, so the value the wake carries would otherwise be lost
     * in `transfer`.  The native records the destination here through
     * ustrand_want_payload, and uexec_run_inner delivers `transfer` into
     * it on the way back in.  An unwind consumes `transfer` instead and
     * clears this. */
    uint32_t   resume_slot;
    struct UCRoot { UValue *slot; struct UCRoot *prev; } *croots;   /* C-root stack for natives */
} UStrand;

/* Created dormant: PARKED, waiting_on NULL, no frames, no stack/frames/
 * cleanup arrays allocated yet -- those grow lazily on first push, so an
 * idle strand stays small. May collect (via ugc_alloc); root `realm`
 * first if it isn't otherwise reachable. */
UStrand *ustrand_new(struct UVM *vm, struct URealm *realm);
/* proto may be NULL (native closure). May collect; root `proto_obj`/proto's
 * owner first if it isn't otherwise reachable -- proto itself is not a GC
 * cell and needs no rooting. */
UClosure *uclosure_new(struct UVM *vm, UProto *proto, uint8_t nupvals);

/* Grows frames[] (from 4, doubling) and the register stack (via
 * ustrand_ensure_stack) to fit base + proto->max_reg + 1 registers (just
 * base + 1 for a native closure), zero-fills that frame's register window
 * to nil, and pushes the frame. 0 ok, -1 OOM (no frame pushed). */
int      ustrand_push_frame(UStrand *s, UClosure *cl, UValue recv, uint32_t base, uint8_t ret_reg);
/* As ustrand_push_frame, but leaves the first `nkeep` registers of the
 * new window untouched instead of nil-filling them.  OP_CALL places the
 * callee's arguments at exactly those slots (the caller's R[A+1..]), so
 * the callee window overlaps them and a blanket nil-fill would erase the
 * arguments it is being handed. */
int      ustrand_push_frame_args(UStrand *s, UClosure *cl, UValue recv, uint32_t base,
                                 uint8_t ret_reg, uint8_t nkeep);
/* Closes any upvalues opened within the top frame's register window, then
 * pops it. No-op on an empty strand. */
void     ustrand_pop_frame(UStrand *s);
/* Grows the register stack by doubling (starting at 32) until stack_cap >=
 * needed, then rewrites every open upvalue's ptr to the (possibly moved)
 * new stack base -- no register pointer may be held across this call.
 * Newly grown capacity reads as nil (ugc_raw_realloc zero-fills it).
 * 0 ok (including when already big enough), -1 OOM. */
int      ustrand_ensure_stack(UStrand *s, uint32_t needed);
/* Appends to the cleanup stack (from 4, doubling). 0 ok, -1 OOM. */
int      ustrand_push_cleanup(UStrand *s, UCleanup c);
/* Finds an already-open upvalue at stack_index, or opens a new one
 * pointing at stack + stack_index, keeping open_upvals sorted by
 * descending stack_index. May collect (via ugc_alloc); NULL on OOM. */
UUpval  *ustrand_find_or_open_upval(UStrand *s, uint32_t stack_index);
/* Closes every open upvalue with stack_index >= from_index: copies the
 * live value into `closed` and repoints ptr at it. Relies on the
 * descending sort to stop at the first surviving (smaller-index) entry. */
void     ustrand_close_upvals(UStrand *s, uint32_t from_index);
void     ustrand_trace(struct UVM *vm, UStrand *s);
void     ustrand_finalize(struct UVM *vm, UStrand *s);

/* C-root helpers for natives that allocate while holding a value. */
#define USTRAND_ROOT(s, var) struct UCRoot _r_##var = { &(var), (s)->croots }; (s)->croots = &_r_##var
#define USTRAND_UNROOT(s, var) (s)->croots = _r_##var.prev
#endif
