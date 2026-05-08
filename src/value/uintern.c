/* SPDX-License-Identifier: BSD-3-Clause */
/* Per-VM string interning. Open-addressing, FNV-1a, grow-by-2 at L>0.7.
 * Freestanding: no malloc, no <string.h> heap users.
 *
 * Storage layout:
 *   UInternTable  - the table struct (single allocation)
 *     entries[]   - cap-sized array of pointers to UInternStr
 *   UInternStr    - per-string allocation: length-prefixed bytes + NUL
 *     hash, len, bytes[len+1]
 *
 * No unintern at v1.0. All UInternStr blocks freed at uintern_destroy.
 *
 * U-prefix on file-private types (UInternStr / UInternTable):
 *   These types are file-private (not exported in the public header) but
 *   carry the U prefix for historical reasons — at M3 they straddled the
 *   uintern.c / uvm.h boundary as cross-TU types, then v0.5.3 layout
 *   relocated their definitions here as the public surface narrowed to
 *   `vm->intern_table` (void *) + the four functions in uintern.h.  The
 *   prefix is retained to keep the type-name search path stable across
 *   the v0.5.x cleanup ramp; renaming would touch every grep history hit
 *   without functional benefit (FOUND-021, v0.5.5). */

#include "value/uintern.h"

#include <stdint.h>
#include <stddef.h>

#include "vm/uvm.h"
#include "gc/ugc.h"

#define INTERN_INITIAL_CAP    16U     /* power of two */
#define INTERN_LOAD_NUM       7U      /* grow when count*10 > cap*7 */
#define INTERN_LOAD_DEN       10U
#define INTERN_TOMBSTONE      ((UInternStr *)1)   /* unused at v1.0; kept for future unintern */

typedef struct UInternStr {
    uint32_t hash;
    uint32_t len;
    char     bytes[1];                /* trailing flexible array; allocated with sizeof(struct) + len */
} UInternStr;

typedef struct UInternTable {
    UInternStr **entries;             /* cap pointers; NULL = empty */
    size_t       cap;                 /* power of two */
    size_t       count;               /* live entries */
} UInternTable;

/* --- helpers --- */

static uint32_t fnv1a(const char *bytes, size_t nbytes) {
    uint32_t h = 0x811c9dc5U;
    for (size_t i = 0; i < nbytes; i++) {
        h ^= (uint8_t)bytes[i];
        h *= 0x01000193U;
    }
    return h;
}

static int bytes_equal(const char *a, size_t alen, const char *b, size_t blen) {
    if (alen != blen) return 0;
    for (size_t i = 0; i < alen; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static void *vm_alloc(UVM *vm, void *ptr, size_t nbytes) {
    if (vm->alloc_fn != NULL) {
        return vm->alloc_fn(ptr, nbytes, vm->alloc_ud);
    }
    /* Defensive guard: alloc_fn is required and is wired by urbi_vm_init.
     * No hosted fallback exists in freestanding builds; if alloc_fn is NULL
     * the caller skipped initialisation.  Returning NULL forces propagation
     * as OOM rather than crashing on a NULL function-pointer call. */
    return NULL;
}

/* Look up bytes/len in t; if found, return the canonical pointer. If not
 * found, return NULL and write the empty-slot index to *out_idx. Probing
 * is linear (open addressing with power-of-two cap and bitwise AND). */
static const char *table_lookup(UInternTable *t, const char *bytes, size_t nbytes,
                                uint32_t hash, size_t *out_idx)
{
    size_t mask = t->cap - 1;
    size_t i = (size_t)hash & mask;
    while (1) {
        UInternStr *e = t->entries[i];
        if (e == NULL) {
            *out_idx = i;
            return NULL;
        }
        if (e != INTERN_TOMBSTONE
            && e->hash == hash
            && bytes_equal(e->bytes, e->len, bytes, nbytes)) {
            return e->bytes;
        }
        i = (i + 1) & mask;
    }
}

static int table_init(UVM *vm, UInternTable *t, size_t cap) {
    size_t bytes = cap * sizeof(UInternStr *);
    /* TIDY-005: explicit cast on void * → UInternStr ** decay. */
    UInternStr **arr = (UInternStr **)vm_alloc(vm, NULL, bytes);
    if (arr == NULL) return 0;
    for (size_t i = 0; i < cap; i++) arr[i] = NULL;
    t->entries = arr;
    t->cap = cap;
    t->count = 0;
    return 1;
}

static void table_reinsert(UInternTable *t, UInternStr *e) {
    size_t mask = t->cap - 1;
    size_t i = (size_t)e->hash & mask;
    while (t->entries[i] != NULL) {
        i = (i + 1) & mask;
    }
    t->entries[i] = e;
}

static int table_grow(UVM *vm, UInternTable *t) {
    size_t old_cap = t->cap;
    UInternStr **old_entries = t->entries;
    size_t new_cap = old_cap * 2;

    /* TIDY-005: explicit cast on void * → UInternStr ** decay. */
    UInternStr **new_arr = (UInternStr **)vm_alloc(vm, NULL, new_cap * sizeof(UInternStr *));
    if (new_arr == NULL) return 0;
    for (size_t i = 0; i < new_cap; i++) new_arr[i] = NULL;

    t->entries = new_arr;
    t->cap = new_cap;
    t->count = 0;

    for (size_t i = 0; i < old_cap; i++) {
        UInternStr *e = old_entries[i];
        if (e != NULL && e != INTERN_TOMBSTONE) {
            table_reinsert(t, e);
            t->count++;
        }
    }
    /* TIDY-005: explicit (void *) cast on UInternStr ** → void * inout decay. */
    vm_alloc(vm, (void *)old_entries, 0);     /* free old array */
    return 1;
}

/* --- public API --- */

const char *ustr_intern(UVM *vm, const char *bytes, size_t nbytes) {
    if (vm == NULL) return NULL;

    /* Lazy table init. */
    UInternTable *t = (UInternTable *)vm->intern_table;
    if (t == NULL) {
        t = vm_alloc(vm, NULL, sizeof(UInternTable));
        if (t == NULL) return NULL;
        if (!table_init(vm, t, INTERN_INITIAL_CAP)) {
            vm_alloc(vm, t, 0);
            return NULL;
        }
        vm->intern_table = t;
    }

    /* FOUND-008: lookup-first ordering.  Compute hash, probe the table once;
     * if the string is already interned, return the canonical pointer
     * unconditionally — the lookup-only fast path must never trigger a
     * grow.  Only when an insertion is actually needed do we re-check the
     * load factor and grow if required. */
    uint32_t hash = fnv1a(bytes, nbytes);
    size_t empty_idx;
    const char *existing = table_lookup(t, bytes, nbytes, hash, &empty_idx);
    if (existing != NULL) return existing;

    /* Insertion path: grow if load factor would be exceeded. */
    if ((t->count + 1) * INTERN_LOAD_DEN > t->cap * INTERN_LOAD_NUM) {
        if (!table_grow(vm, t)) return NULL;
        /* Re-probe in the resized table — the empty slot index from the
         * pre-grow lookup is invalid against the new cap. */
        existing = table_lookup(t, bytes, nbytes, hash, &empty_idx);
        if (existing != NULL) return existing;
    }

    /* Allocate new entry: header + nbytes + NUL terminator. */
    size_t alloc_bytes = sizeof(UInternStr) + nbytes;   /* +1 NUL absorbed by bytes[1] */
    UInternStr *e = vm_alloc(vm, NULL, alloc_bytes);
    if (e == NULL) return NULL;
    e->hash = hash;
    e->len = (uint32_t)nbytes;
    for (size_t i = 0; i < nbytes; i++) e->bytes[i] = bytes[i];
    e->bytes[nbytes] = '\0';

    t->entries[empty_idx] = e;
    t->count++;
    return e->bytes;
}

void uintern_destroy(UVM *vm) {
    if (vm == NULL) return;
    UInternTable *t = (UInternTable *)vm->intern_table;
    if (t == NULL) return;

    for (size_t i = 0; i < t->cap; i++) {
        UInternStr *e = t->entries[i];
        if (e != NULL && e != INTERN_TOMBSTONE) {
            vm_alloc(vm, e, 0);
        }
    }
    /* TIDY-005: explicit (void *) cast on UInternStr ** → void * inout decay. */
    vm_alloc(vm, (void *)t->entries, 0);
    vm_alloc(vm, t, 0);
    vm->intern_table = NULL;
}

size_t uintern_count(UVM *vm) {
    if (vm == NULL || vm->intern_table == NULL) return 0;
    return ((UInternTable *)vm->intern_table)->count;
}

/* === intern_table_walk_roots ===
 *
 * GC root provider for the intern table (row 10 §5.5).
 *
 * No-op by design through v1.0.  Interned strings are stored as raw
 * `const char *` pointers inside UInternStr heap allocations — they are
 * NOT GC-managed UValues, and the intern table owns the UInternStr blocks
 * directly (freed at uintern_destroy via vm->alloc_fn).  Strong ownership
 * by the table means the GC has nothing to keep alive on its behalf, so
 * this walker reports zero roots and does no work.
 *
 * The function is registered as a root provider only so that the provider
 * slot in vm->gc is occupied and the dispatch path is exercised, keeping
 * the M4-shipped GC root-provider API symmetric across all subsystems.
 *
 * If a future revision migrates strings to a UString GC cell type, this
 * walker would iterate occupied non-tombstone entries and report each as
 * a UValue root via cb — at which point intern lifetime would track the
 * GC cycle rather than the table allocation.  Filed under v1.x backlog
 * (FOUND-024 disposition, v0.5.5: no-op-by-design at v1.0). */
void
intern_table_walk_roots(UVM *vm, UGcRootCallback cb, void *ctx)
{
    /* No-op by design — see banner above. */
    (void)vm;
    (void)cb;
    (void)ctx;
}
