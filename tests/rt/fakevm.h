#ifndef FAKEVM_H
#define FAKEVM_H
#include <stdlib.h>
#include <string.h>
#include "rt/uexec.h"
/* Thin wrapper over the real VM.  It exists only so the rt suites can
 * (a) hand the collector an extra array of roots and (b) keep the VM in
 * an automatic variable instead of going through uvm_open's heap
 * allocation.  Everything else — the GC hooks, the string table, the
 * object-id counter — is the production wiring from src/rt/uexec.c. */

typedef struct FakeRoots { UCell **roots; int n; } FakeRoots;
static FakeRoots fakevm_roots;

static void *fake_alloc(void *p, size_t n, void *ud) {
    (void)ud;
    if (n == 0) { free(p); return NULL; }
    return realloc(p, n);
}

static void fake_mark_extra(struct UVM *vm, void *ud) {
    FakeRoots *fr = (FakeRoots *)ud;
    for (int i = 0; i < fr->n; i++) ugc_mark(vm, fr->roots[i]);
}

/* As fakevm_init, over a caller's allocator -- one that counts, say. */
static inline void fakevm_init_with(struct UVM *vm, UCell **roots, int n, UAllocFn alloc, void *ud) {
    memset(vm, 0, sizeof *vm);
    ugc_init(&vm->gc, alloc, ud);
    vm->gc.hooks.mark_fixed = uvm_gc_mark_fixed;
    vm->gc.hooks.trace      = uvm_gc_trace;
    vm->gc.hooks.finalize   = uvm_gc_finalize;
    ustrtab_init(vm, &vm->strings);
    fakevm_roots.roots = roots;
    fakevm_roots.n = n;
    vm->test_mark_extra = fake_mark_extra;
    vm->test_mark_ud = &fakevm_roots;
}

static inline void fakevm_init(struct UVM *vm, UCell **roots, int n) {
    fakevm_init_with(vm, roots, n, fake_alloc, NULL);
}

static inline void fakevm_destroy(struct UVM *vm) {
    vm->test_mark_extra = NULL;
    ugc_destroy(vm);
    ustrtab_destroy(vm, &vm->strings);
}
#endif
