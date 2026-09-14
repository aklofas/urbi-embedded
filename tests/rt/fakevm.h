#ifndef FAKEVM_H
#define FAKEVM_H
#include <stdlib.h>
#include "rt/ugc.h"
#include "rt/ustr.h"
/* Until uexec.h exists, tests define struct UVM themselves; Task 8 replaces
 * this header with one that includes rt/uexec.h and keeps the same helpers. */
struct UVM { UGc gc; UCell **roots; int nroots; UStrTab strings; };
static void *fake_alloc(void *p, size_t n, void *ud) { (void)ud; if (n == 0) { free(p); return NULL; } return realloc(p, n); }
static void fake_mark_fixed(struct UVM *vm) { for (int i = 0; i < vm->nroots; i++) ugc_mark(vm, vm->roots[i]); }
static void fake_trace(struct UVM *vm, UCell *c) { (void)vm; (void)c; }
static void fake_finalize(struct UVM *vm, UCell *c) { (void)vm; (void)c; }
static inline void fakevm_init(struct UVM *vm, UCell **roots, int n) {
    ugc_init(&vm->gc, fake_alloc, NULL);
    vm->gc.hooks.mark_fixed = fake_mark_fixed; vm->gc.hooks.trace = fake_trace; vm->gc.hooks.finalize = fake_finalize;
    vm->roots = roots; vm->nroots = n;
    ustrtab_init(vm, &vm->strings);
}
static inline void fakevm_destroy(struct UVM *vm) {
    ustrtab_destroy(vm, &vm->strings);
    ugc_destroy(vm);
}
#endif
