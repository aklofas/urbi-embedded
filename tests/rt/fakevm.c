#include "fakevm.h"
/* The real accessor lives in src/rt/uexec.c (Task 8); until then the rt
 * test runner links this one definition. Task 8 deletes this file. */
UGc *uvm_gc(struct UVM *vm) { return &vm->gc; }
