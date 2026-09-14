/* SPDX-License-Identifier: BSD-3-Clause */
/* src/rt/ugc.h — cells, allocator, roots, and the stop-the-world mark-sweep
 * collector for the refound/core runtime. */

#ifndef URT_GC_H
#define URT_GC_H
#include "rt/uvalue.h"

typedef void *(*UAllocFn)(void *ptr, size_t nbytes, void *ud);   /* realloc-shaped; nbytes==0 frees */

typedef enum { UCELL_STR = 1, UCELL_OBJ, UCELL_CLOSURE, UCELL_UPVAL, UCELL_LIST, UCELL_DICT,
               UCELL_TAG, UCELL_EVENT, UCELL_STRAND, UCELL_WATCHER, UCELL_PROTO, UCELL_PROPS,
               UCELL_REALM, UCELL_HOST } UCellType;

typedef struct UCell {
    struct UCell *next;
    uint32_t size;
    uint8_t  type;       /* UCellType */
    uint8_t  marked;      /* tri-state: 0 white, 1 gray (queued, not yet traced), 2 black (traced) */
    uint16_t flags;      /* per-type bits; the two pin bits are reserved here */
} UCell;
/* The HOST's pin, taken by urbi_ref and released by urbi_unref.  The
 * runtime never sets or clears it -- that is the guarantee <urbi/urbi.h>
 * makes, and the reason the internal pin below is a separate bit. */
#define UCELL_F_PINNED 0x8000
/* The RUNTIME's pin: a short-lived hold on a cell that is reachable from
 * nothing yet (a fresh chunk, a fresh closure) or reachable only through a
 * caller's register, taken across an allocation that may collect.
 *
 * NOT NESTABLE.  It is one bit, so an inner release clears an outer hold;
 * every site that takes it must therefore release it before returning, and
 * no two live holds may name the same cell.  A hold that has to outlive a
 * call goes on the strand's C-root stack instead (see USTRAND_ROOT). */
#define UCELL_F_RTPIN  0x4000
/* Either pin keeps a cell through sweep; ugc_collect pre-marks both. */
#define UCELL_F_PIN_ANY (UCELL_F_PINNED | UCELL_F_RTPIN)

struct UVM;
typedef struct UGcRoots {              /* fixed roots the VM registers once */
    void (*mark_fixed)(struct UVM *vm);           /* marks realms, run queue, timers, watchers, protos */
    void (*trace)(struct UVM *vm, UCell *c);      /* marks a cell's children by type */
    /* finalize: frees a cell's owned non-cell memory (typically via
     * ugc_raw_free). MUST NOT allocate -- directly, via ugc_alloc, or via
     * ugc_raw_alloc/realloc -- because the collector is mid-sweep while
     * finalize runs and reentrant allocation would corrupt the in-progress
     * cell list and gray-stack bookkeeping. Debug builds enforce this with
     * UGC_ASSERT(!g->in_collect) at the top of ugc_alloc/ugc_raw_alloc. */
    void (*finalize)(struct UVM *vm, UCell *c);
} UGcRoots;

typedef struct UGc {
    UAllocFn alloc; void *alloc_ud;
    UCell   *all;                 /* intrusive all-cells list */
    UCell  **gray; uint32_t gray_len, gray_cap;   /* explicit mark stack (no recursion) */
    size_t   bytes_live, bytes_since, threshold;
    size_t   raw_live;            /* live bytes owned via ugc_raw_* (arrays etc.), not swept as cells */
    uint32_t cycles, cells_live;
    uint8_t  pause_ratio;         /* percent; 200 = collect when since > 2x live */
    uint8_t  in_collect;
    uint8_t  gray_overflow;       /* set when a push onto gray[] fails (OOM); cleared by the fallback rescan */
    UGcRoots hooks;
} UGc;

/* Accessor into the owning VM, defined by the layer above (uexec.c); see
 * tests/rt/fakevm.c for the stand-in used before that layer exists. Keeps
 * this header from depending on uexec.h, which would violate layering. */
UGc *uvm_gc(struct UVM *vm);

/* Debug-only trap for invariants that must never fire in a working build
 * (e.g. reentrant allocation from a finalize hook). src/rt has no
 * <assert.h> dependency by policy, so this is a minimal freestanding-safe
 * substitute: a no-op unless URBI_DEBUG is defined. */
#ifdef URBI_DEBUG
#define UGC_ASSERT(cond) do { if (!(cond)) __builtin_trap(); } while (0)
#else
#define UGC_ASSERT(cond) do { } while (0)
#endif

int    ugc_init(UGc *g, UAllocFn alloc, void *ud);
void   ugc_destroy(struct UVM *vm);                /* frees every cell via finalize + alloc(0) */
void  *ugc_alloc(struct UVM *vm, UCellType type, size_t nbytes);  /* zeroed; NULL on OOM */
void  *ugc_raw_alloc(struct UVM *vm, size_t nbytes);              /* non-cell owned memory (arrays) */
void  *ugc_raw_realloc(struct UVM *vm, void *p, size_t old, size_t nbytes);
void   ugc_raw_free(struct UVM *vm, void *p, size_t nbytes);
void   ugc_mark(struct UVM *vm, UCell *c);          /* push gray if unmarked; O(1) */
void   ugc_mark_value(struct UVM *vm, UValue v);    /* marks STR/OBJ/CELL payloads */
void   ugc_collect(struct UVM *vm);                 /* full stop-the-world cycle */
bool   ugc_should_collect(const UGc *g);
void   ugc_maybe_collect(struct UVM *vm);           /* called at safepoints; also honours URBI_GC_STRESS */
#endif
