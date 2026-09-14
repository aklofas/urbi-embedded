#ifndef FAKEVM_H
#define FAKEVM_H
#include <stdlib.h>
#include "rt/ugc.h"
#include "rt/ustr.h"
#include "rt/uobj.h"
#include "rt/ulist.h"
/* Until uexec.h exists, tests define struct UVM themselves; Task 8 replaces
 * this header with one that includes rt/uexec.h and keeps the same helpers. */
struct UVM { UGc gc; UCell **roots; int nroots; UStrTab strings; UObjStats objstats; };
static void *fake_alloc(void *p, size_t n, void *ud) { (void)ud; if (n == 0) { free(p); return NULL; } return realloc(p, n); }
static void fake_mark_fixed(struct UVM *vm) { for (int i = 0; i < vm->nroots; i++) ugc_mark(vm, vm->roots[i]); }
static void fake_trace(struct UVM *vm, UCell *c) {
    switch (c->type) {
    case UCELL_OBJ: uobj_trace(vm, (UObject *)c); break;
    case UCELL_PROPS: {
        UProps *p = (UProps *)c;
        ugc_mark_value(vm, p->getter);
        ugc_mark_value(vm, p->setter);
        ugc_mark_value(vm, p->value);
        break;
    }
    case UCELL_LIST: ulist_trace(vm, (UList *)c); break;
    case UCELL_DICT: udict_trace(vm, (UDict *)c); break;
    default: break;
    }
}
static void fake_finalize(struct UVM *vm, UCell *c) {
    switch (c->type) {
    case UCELL_OBJ: uobj_finalize(vm, (UObject *)c); break;
    case UCELL_LIST: ulist_finalize(vm, (UList *)c); break;
    case UCELL_DICT: udict_finalize(vm, (UDict *)c); break;
    default: break;
    }
}
static inline void fakevm_init(struct UVM *vm, UCell **roots, int n) {
    ugc_init(&vm->gc, fake_alloc, NULL);
    vm->gc.hooks.mark_fixed = fake_mark_fixed; vm->gc.hooks.trace = fake_trace; vm->gc.hooks.finalize = fake_finalize;
    vm->roots = roots; vm->nroots = n;
    ustrtab_init(vm, &vm->strings);
    vm->objstats.next_id = 0; vm->objstats.visit_stamp = 0;
}
static inline void fakevm_destroy(struct UVM *vm) {
    ustrtab_destroy(vm, &vm->strings);
    ugc_destroy(vm);
}
#endif
