#include "fakevm.h"
/* The real accessors live in src/rt/uexec.c (Task 8); until then the rt
 * test runner links these definitions. Task 8 deletes this file. */
UGc *uvm_gc(struct UVM *vm) { return &vm->gc; }
UStrTab *uvm_strings(struct UVM *vm) { return &vm->strings; }
