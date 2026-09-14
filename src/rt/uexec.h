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
       UP_DURATION, UP_REGEXP, UP_MUTEX, UP_PAIR, UP_TRIPLET, UP_TUPLE, UP_DEBUG, UP_GLOBAL,
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
};

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
     * ustr_intern(vm, ...), declared by src/value/uintern.h.  The new
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

/* Builds an exception object of protos[which] with a "message" slot,
 * deposits it in s->transfer and sets s->unwind = UUNWIND_THROW.
 * Always returns UEXEC_THROW so callers can `return uexec_throw(...)`. */
int uexec_throw(UVM *vm, UStrand *s, int which_proto, const char *msg);
int uexec_throw_value(UVM *vm, UStrand *s, UValue v);
/* Formats the pending throw into vm->last_error and kills the strand.
 * Returns 1 (the strand is no longer runnable).  The cleanup-stack
 * walker replaces the body of this in the unwind task. */
int uexec_unwind(UVM *vm, UStrand *s);

/* --- running --------------------------------------------------------- */

/* Run a closure synchronously on the given strand: pushes a boundary
 * frame and runs until that frame returns.  URBI_OK / UEXEC_THROW. */
int uexec_call(UVM *vm, UStrand *s, UClosure *cl, UValue recv, const UValue *argv, uint8_t argc, UValue *out);
/* Run a strand until it parks, dies, or the budget is spent.  Returns
 * the strand's new state (USTRAND_READY / PARKED / DEAD / RUNNING). */
int uexec_run(UVM *vm, UStrand *s, uint32_t budget);

UStrand *uvm_spare_acquire(UVM *vm, URealm *realm);
void     uvm_spare_release(UVM *vm, UStrand *s);

/* Compile + bind + run source on a realm's globals; *out is the value of
 * the last statement.  URBI_OK, URBI_ERR_COMPILE (err holds the
 * diagnostic), URBI_ERR_OOM, or URBI_ERR_UNCAUGHT_THROW (vm->last_error
 * holds the message). */
int uexec_run_source(UVM *vm, URealm *realm, const char *src, size_t n,
                     const char *name, UValue *out, char *err, size_t errcap);

/* GC hooks — exported so tests/rt/fakevm.h can reuse the real ones. */
void uvm_gc_mark_fixed(UVM *vm);
void uvm_gc_trace(UVM *vm, UCell *c);
void uvm_gc_finalize(UVM *vm, UCell *c);

#endif
