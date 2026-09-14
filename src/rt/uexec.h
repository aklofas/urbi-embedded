/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/uexec.h — the VM struct, chunk binding, closures, and the
 * bytecode dispatch entry points for the refound/core runtime.
 *
 * This is the one header that sees every layer below it.  It completes
 * `struct UVM`, which ugc.h / ustr.h / uobj.h / ustrand.h only forward
 * declare and reach through the four accessors defined in uexec.c. */

#ifndef URT_EXEC_H
#define URT_EXEC_H
#include "rt/usched.h"
#include "chunk/uproto.h"   /* the loader's UProto — the only old-tree header rt/ includes */

/* --- built-in prototype slots -------------------------------------- */

enum { UP_OBJECT = 0, UP_INTEGER, UP_FLOAT, UP_STRING, UP_BOOLEAN, UP_NIL, UP_VOID, UP_LIST, UP_DICT, UP_SYMBOL,
       UP_CLOSURE, UP_TAG, UP_EVENT, UP_STRAND /* Job */, UP_EXCEPTION, UP_TYPEERROR, UP_ARITYERROR, UP_LOOKUPERROR,
       UP_OOMERROR, UP_INDEXERROR, UP_RANGEERROR, UP_DIVBYZERO, UP_LOBBY, UP_CHANNEL, UP_MATH, UP_SYSTEM, UP_DATE,
       UP_DURATION, UP_REGEXP, UP_MUTEX, UP_PAIR, UP_TRIPLET, UP_TUPLE, UP_DEBUG, UP_GLOBAL, UP_CALLMESSAGE,
       UP_COUNT };

/* --- realm ----------------------------------------------------------
 *
 * One script world.  It is a GC cell because UStrand.realm is marked
 * through as one (ustrand_trace).
 *
 * The struct is completed HERE rather than in rt/urealm.h because the
 * dispatch loop dereferences `s->realm->globals` on the
 * OP_LOAD_REALM_GLOBAL path, and uexec sits below urealm in the include
 * order — uexec.h may not include rt/urealm.h.  The realm's behaviour
 * (creation, teardown, the writer) lives in rt/urealm.h; only the shape
 * is here. */
struct URealm {
    UCell          cell;
    UVM           *vm;             /* owning VM; lets a native reach the VM from a realm handle */
    UObject       *globals;        /* this realm's globals; vm->root_globals is its proto */
    struct UTag   *root_tag;       /* NULL until the scheduler task; stopping it kills the realm's strands */
    UStrand       *strands;        /* threaded via UStrand.next_in_realm */
    struct URealm *next;           /* vm->realms list */
    /* Per-realm output sink.  NULL falls back to the VM-wide writer. */
    void (*writer)(void *ud, const char *chan, size_t cl, const char *msg, size_t ml);
    void  *writer_ud;
    /* Compile limits for source text this realm runs.  All-zero (the
     * state urealm_new leaves it in) means unlimited, which is what a
     * host running its own code wants; a REPL session sets real numbers,
     * because the text arrives from somewhere else. */
    UCompileBudget budget;
};

/* --- the watcher set ------------------------------------------------
 *
 * The reactive runtime's per-VM state.  Like URealm, only the SHAPE is
 * here -- `UWatchState` is a by-value member of `struct UVM` below, so it
 * has to be complete before the VM is, while rt/uwatch.h sits ABOVE this
 * header and carries UWatcher and every function that touches one.
 *
 * THE DIRTY SET IS A COUNT, not a list of objects.  `UOBJ_F_WATCHED` is
 * sticky and per-object, so once a condition has read a realm's globals
 * every global write marks dirty and every drain re-evaluates every armed
 * condition anyway; an object array would be a second copy of that fact
 * with a rooting problem attached (a dirty-marked object the script has
 * dropped would either be kept alive or dangle).  `ndirty` is therefore
 * the number of writes to watched objects since the last drain, and the
 * drain's question is only "any?". */
typedef struct UWatcher UWatcher;

/* The five shapes a watcher comes in.  Here rather than in rt/uwatch.h
 * because the install opcodes name them and the dispatch loop is
 * exec-rank; rt/uwatch.h carries everything else about a watcher. */
typedef enum {
    UWATCH_AT = 1,      /* at (cond) / at (e?) -- the body spawns on the rising edge */
    UWATCH_AT_SYNC,     /* at sync (...) -- the body runs inline on a spare strand */
    UWATCH_WHENEVER,    /* whenever (cond) -- re-fires while the condition holds */
    UWATCH_WAITUNTIL,   /* waituntil (cond) -- wakes its waiters once, then dies */
    UWATCH_ONCE         /* one-shot event subscription; dies on its first fire */
} UWatchMode;

typedef struct UWatchState {
    UWatcher *all;        /* every live watcher; a fixed GC root */
    uint32_t  ndirty;     /* writes to watched objects since the last drain */
    uint8_t   draining;   /* a drain or an event fan-out is walking the lists */
    uint8_t   observing;  /* a condition is running: slot reads mark their object */
} UWatchState;

/* --- bound chunk ----------------------------------------------------
 *
 * One cell per loaded chunk root.  Binding interns the chunk's IC names
 * and its string constants into USym, so nothing in a bound proto tree
 * points at the frontend's arena or at loader-owned bytes any more.
 *
 * The cell is reachable from every closure over any proto in the tree:
 * uproto_root_of(cl->proto)->owning_module_instance is the back-pointer
 * (the field is reused; the old module-instance machinery is gone).  A
 * chunk therefore stays alive exactly as long as some closure, frame or
 * strand still refers to it, and is swept otherwise. */
typedef struct UProtoCell {
    UCell              cell;
    UProto            *root;
    struct UProtoCell *next_bound;   /* vm->bound_protos list */
} UProtoCell;

/* --- the VM ---------------------------------------------------------- */

struct UVM {
    UGc        gc;
    UStrTab    strings;
    UObjStats  objstats;
    USched     sched;
    UWatchState watch;
    UObject   *protos[UP_COUNT];
    UObject   *root_globals;       /* built-in globals; every realm's globals inherits from it */
    URealm    *realms;             /* list; realms->... ; main_realm is the first created */
    URealm    *main_realm;
    UStrand   *spare;              /* free list of spare strands for synchronous runs */
    UStrand   *spare_active;       /* spares currently handed out; a GC root (linked via UStrand.link) */
    UProtoCell *bound_protos;      /* every live bound chunk, for uvm_close teardown */

    /* host hooks */
    uint64_t (*clock_us)(void *ud); void *clock_ud;
    void (*diag)(struct UVM *vm, void *ud, int level, const char *msg, size_t len); void *diag_ud;
    void (*writer)(void *ud, const char *chan, size_t cl, const char *msg, size_t ml); void *writer_ud;
    void (*wake)(void *ud); void *wake_ud;

    /* last error that escaped a top frame */
    char       last_error[256];
    int        last_error_code;

    /* Frontend seam: the emitter interns identifier names through
     * ustr_intern(vm, ...), declared by src/emit/uintern.h.  The new
     * core implements that function over the USym table above, so
     * `strings` IS the intern table and there is no second one.  See
     * src/emit/ufront.h. */

    uint8_t    stdlib_booted;

    /* Test-only extra root provider (tests/rt/fakevm.h).  NULL in every
     * shipping configuration; mark_fixed calls it when set. */
    void     (*test_mark_extra)(struct UVM *vm, void *ud);
    void      *test_mark_ud;
};

/* --- lifecycle ------------------------------------------------------- */

UVM  *uvm_open(UAllocFn alloc, void *ud);   /* allocates the VM through alloc; boots nothing */
void  uvm_close(UVM *vm);

UGc       *uvm_gc(UVM *vm);
UStrTab   *uvm_strings(UVM *vm);
UObjStats *uvm_objstats(UVM *vm);
USched    *uvm_sched(UVM *vm);
UWatchState *uvm_watch(UVM *vm);

static inline uint8_t uproto_max_reg(const UProto *p) { return p ? p->max_reg : 0; }

/* --- realms ----------------------------------------------------------
 *
 * Declared here only because uvm_gc_trace dispatches UCELL_REALM to it;
 * defined in urealm.c, alongside the rest of the realm API (rt/urealm.h). */
void    urealm_trace(UVM *vm, URealm *r);

/* --- closures and chunks --------------------------------------------- */

/* Native closure.  `fn` keeps the stdlib's urbi_native_method_fn shape. */
UClosure  *uclosure_native(UVM *vm, int (*fn)(UVM *, UValue, UValue *, uint8_t, UValue *),
                           uint8_t min_args, uint8_t max_args);

/* Bind a loaded chunk root to this VM: interns every proto's ic names
 * into USym (rewriting proto->ic_names in place), rewrites every UVAL_STR
 * constant into a UV_SYM value, and wraps the root in a UCELL_PROTO cell
 * whose finaliser calls uchunk_destroy.  Takes ownership of `root`
 * unconditionally: on OOM it returns NULL having already released (or
 * handed to a soon-collected cell) the chunk, so the caller must never
 * destroy `root` itself after calling this. */
UProtoCell *uproto_bind(UVM *vm, UProto *root);
/* The USym array for a proto's IC sites, or NULL when it has none. */
static inline USym **uproto_names(const UProto *p) { return (USym **)p->ic_names; }

/* The object slot lookups start from, for any value kind.  Task 8 rule:
 * OBJ receivers only; every other kind returns NULL and the caller
 * throws TypeError until the stdlib prototypes exist. */
UObject *uv_dispatch_proto(UVM *vm, UValue recv);

/* --- errors ---------------------------------------------------------- */

/* Builds an exception object of protos[which] with "name", "message" and
 * "line" slots, deposits it in s->transfer and sets s->unwind =
 * UUNWIND_THROW.  The message slot reads "<TypeName>: <msg>", which is
 * what an uncaught throw prints and what a catch handler sees in
 * e.message.  Always returns UEXEC_THROW so callers can
 * `return uexec_throw(...)`. */
int uexec_throw(UVM *vm, UStrand *s, int which_proto, const char *msg);
/* As uexec_throw, but the message also carries the source position of the
 * instruction being executed ("line 7: TypeError: ...").  Reserved for the
 * failures the corpus pins a position on: the arithmetic, comparison and
 * division-by-zero raises the dispatch loop makes on its own behalf. */
int uexec_throw_here(UVM *vm, UStrand *s, int which_proto, const char *msg);
int uexec_throw_value(UVM *vm, UStrand *s, UValue v);

/* The cleanup-stack walker (rt/uunwind.c).  Consumes s->unwind and
 * s->transfer.  Returns 0 when dispatch can continue in the current frame
 * (a catch was entered, a finally body was started, or a return completed
 * into the caller), 1 when the strand is DEAD or control must go back to
 * uexec_call. */
int uexec_unwind(UVM *vm, UStrand *s);
/* Completes a return out of the top frame: closes its upvalues, pops it
 * and delivers `rv`.  0 = dispatch continues in the caller, 1 = the run
 * is over (s->state and s->result are set). */
int uexec_return(UVM *vm, UStrand *s, UValue rv);
/* Source line for an instruction index within one proto, or 0 when the
 * proto carries no line table.  Defined in rt/uunwind.c. */
uint32_t uproto_line_at(const UProto *p, uint32_t pc);
/* Source line of the instruction the top frame is executing, or 0. */
uint32_t uexec_current_line(UStrand *s);
/* Appends "line N: " (or "<source>:N: ") for `line`, or nothing when it
 * is 0.  Returns the new length. */
size_t uexec_position_prefix(UStrand *s, uint32_t line, char *buf, size_t cap, size_t at);

/* Maps a UEXEC_* result from a top-level entry point (urbi_run,
 * urbi_call, urbi_load) onto the public URBI_* code, and settles
 * vm->last_error: cleared on success, left holding the escaped value's
 * rendering on a throw.  Every one of those entry points goes through
 * this, so none of them can report the same failure differently. */
int uexec_finish_run(UVM *vm, int exec_rc);

/* --- running --------------------------------------------------------- */

/* Run a closure synchronously on the given strand: pushes a boundary
 * frame and runs until that frame returns.  URBI_OK / UEXEC_THROW. */
int uexec_call(UVM *vm, UStrand *s, UClosure *cl, UValue recv, const UValue *argv, uint8_t argc, UValue *out);
/* Run a strand until it parks, dies, or the budget is spent.  Returns
 * the strand's new state (USTRAND_READY / PARKED / DEAD / RUNNING). */
int uexec_run(UVM *vm, UStrand *s, uint32_t budget);

UStrand *uvm_spare_acquire(UVM *vm, URealm *realm);
void     uvm_spare_release(UVM *vm, UStrand *s);

/* Run a bound chunk's root closure as a scheduled strand of `realm`,
 * under the realm's connection tag, and pump the scheduler until nothing
 * is READY.  *out is the chunk's value, or nil when its strand is still
 * parked.  URBI_OK or URBI_ERR_OOM / URBI_ERR_UNCAUGHT_THROW. */
int uexec_run_chunk(UVM *vm, URealm *realm, UClosure *cl, UValue *out);

/* Compile + bind + run source on a realm's globals; *out is the value of
 * the last statement.  URBI_OK, URBI_ERR_COMPILE (err holds the
 * diagnostic), URBI_ERR_OOM, or URBI_ERR_UNCAUGHT_THROW (vm->last_error
 * holds the message). */
int uexec_run_source(UVM *vm, URealm *realm, const char *src, size_t n,
                     const char *name, UValue *out, char *err, size_t errcap);

/* Arrange for the payload of the next wake to land in the register the
 * OP_CALL currently running this native writes its result into.  The
 * native reads its own call site: do_call does not push a frame for a
 * native, so the top frame is the CALLER's and the instruction it has
 * just consumed (pc[-1]) is that OP_CALL, whose A field is the
 * destination register.  A no-op on a strand that is not inside one. */
void ustrand_want_payload(UStrand *s);

/* Everything a completed slot write owes the reactive runtime: a watched
 * object re-arms the dirty set, and a slot subscribed to through
 * `x.changed?` fires its event with the value just written.  Defined in
 * uexec_ops.c beside OP_SETSLOT; urbi_slot_set and urbi_global_set call it
 * too, which is what makes a HOST write between two steps wake a
 * `waituntil` on the same slot. */
void uexec_note_write(UVM *vm, UObject *o, const USym *name, UValue v, bool existed);

/* --- provided by the layer above (uwatch) ---------------------------
 *
 * rt/uwatch.h ranks above this header, so the exec core, the scheduler
 * and the GC hooks reach the reactive runtime through these declarations
 * rather than by including it -- the same arrangement as uexec_run being
 * declared in rt/usched.h.  rt/uwatch.h carries the contract for each. */

/* Install a condition watcher (`cond` non-NULL) or an event watcher
 * (`event` non-NULL) on behalf of the strand executing the install
 * opcode.  `body` and `onleave` may be NULL.  NULL on OOM. */
UWatcher *uwatch_install(UVM *vm, UStrand *s, uint8_t mode, UClosure *cond,
                         UEvent *event, UClosure *body, UClosure *onleave);
/* OP_WAITUNTIL_INSTALL: 0 = the condition already held (or the strand may
 * not park) and dispatch continues, 1 = the strand is parked, -1 = the
 * condition threw and s is unwinding. */
int   uwatch_waituntil(UVM *vm, UStrand *s, UClosure *cond);
/* A write landed on an object carrying UOBJ_F_WATCHED. */
void  uwatch_mark_dirty(UVM *vm, UObject *o);
/* A slot read while a condition is running marks its object, so a later
 * write to it re-arms the dirty set.  Inline because every resolved slot
 * read on every strand pays it and the answer is almost always no: the
 * guard is one predictable branch on a field of the VM the caller is
 * already holding. */
static inline void uwatch_observe(const UVM *vm, UObject *o)
{
    if (vm->watch.observing && o) o->cell.flags |= UOBJ_F_WATCHED;
}
/* Evaluate every armed condition watcher once and act on the edges. */
void  uwatch_drain(UVM *vm);
/* Fan `payload` out to `e`'s watchers.  See uevent_emit_to. */
void  uwatch_event_fired(UVM *vm, UEvent *e, UValue payload, bool sync);
/* A strand died: a `whenever` whose body it was re-evaluates and respawns
 * while the condition still holds. */
void  uwatch_body_done(UVM *vm, const UStrand *dead);
/* Cancel every watcher installed under `t`. */
void  uwatch_tag_stopped(UVM *vm, const UTag *t);
/* Cancel every watcher belonging to `r`, for realm teardown. */
void  uwatch_realm_dropped(UVM *vm, const URealm *r);
/* The slot-change event for (o, name), created on first ask.  NULL on
 * OOM. */
UEvent *uwatch_slot_change_event(UVM *vm, UObject *o, const USym *name);
/* A slot with USLOT_CHANGED_EVENT was just written: emit its event with
 * the new value as the payload. */
void  uwatch_slot_changed(UVM *vm, UObject *o, const USym *name, UValue v);
/* A write has just INSTALLED `name` on an object that carries
 * UOBJ_F_CHANGE_EVENTS: attach the marker when that slot is the one
 * somebody subscribed to before it existed.  Installing a slot is never
 * itself a change, so this arms rather than fires. */
void  uwatch_slot_installed(UVM *vm, UObject *o, const USym *name);
/* Whether any armed watcher still has a subscriber that could run. */
bool  uwatch_has_live_work(const UVM *vm);
/* GC: the watcher list (a fixed root) and one watcher's children. */
void  uwatch_mark(UVM *vm);
void  uwatch_trace(UVM *vm, UWatcher *w);

/* GC hooks — exported so tests/rt/fakevm.h can reuse the real ones. */
void uvm_gc_mark_fixed(UVM *vm);
void uvm_gc_trace(UVM *vm, UCell *c);
void uvm_gc_finalize(UVM *vm, UCell *c);

#endif
