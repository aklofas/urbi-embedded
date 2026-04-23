/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode interpreter. */

#include "uvm.h"

#if __STDC_HOSTED__
#  include <stdlib.h>

/* Default allocator: realloc semantics. Only compiled in hosted builds. */
static void *uvm_stdlib_realloc(void *ptr, size_t nbytes, void *ud) {
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}
#endif  /* __STDC_HOSTED__ */

void uvm_init(UVM *vm, UVMAllocFn alloc_fn, void *alloc_ud) {
    vm->alloc_fn = alloc_fn;
    vm->alloc_ud = alloc_ud;
#if __STDC_HOSTED__
    if (vm->alloc_fn == NULL) {
        vm->alloc_fn = uvm_stdlib_realloc;
        vm->alloc_ud = NULL;
    }
#endif
    vm->last_error = UVM_OK;
    vm->last_errmsg[0] = '\0';
}

void uvm_destroy(UVM *vm) {
    /* Nothing to free at M1 — register frames are allocated and freed
       within a single uvm_run call, not held across lifecycle. */
    (void)vm;
}

const char *uvm_error_name(UVMError code) {
    switch (code) {
        case UVM_OK:         return "UVM_OK";
        case UVM_TYPE_ERROR: return "UVM_TYPE_ERROR";
        case UVM_OOM:        return "UVM_OOM";
    }
    return "UVM_UNKNOWN";
}

/* --- Local zero-fill. Volatile byte pointer prevents GCC/Clang from
       recognizing the loop and lowering it to a memset libcall under
       -Os, which would break freestanding builds.
       Matches uarena.c's arena_zero pattern precisely. --- */
static void vm_zero(void *const dst, const size_t n) {
    volatile unsigned char *const p = (volatile unsigned char *)dst;
    for (size_t i = 0; i < n; i++) p[i] = 0;
}

/* --- Dispatch macros. Switch expansion at this stage; computed-goto
       added in a later task. --- */

#define DISPATCH()  switch (uinstr_op(*pc))
#define CASE(op)    case (op):
#define NEXT()      do { pc++; goto dispatch; } while (0)
#define HALT()      goto halt

/* --- uvm_run --- */

UVMError uvm_run(UVM *vm, const Chunk *chunk, UConst *out) {
    /* Initialize out to Nil; overwritten on OP_RET success. */
    UConst nil = {0};  /* kind = UVAL_NIL, payload zeroed */
    *out = nil;

    /* Empty chunk: no instructions to dispatch; return Nil. */
    if (chunk->instr_count == 0) {
        return UVM_OK;
    }

    /* Allocate the register frame via the VM's allocator hook.
       (max_reg + 1) slots; zero-initialized so every register starts Nil. */
    const size_t frame_slots = (size_t)(chunk->max_reg + 1);
    const size_t frame_bytes = frame_slots * sizeof(UConst);
    UConst *frame = (UConst *)vm->alloc_fn(NULL, frame_bytes, vm->alloc_ud);
    if (frame == NULL) {
        vm->last_error = UVM_OOM;
        vm->last_errmsg[0] = '\0';  /* diagnostic formatting in a later task */
        return UVM_OOM;
    }
    vm_zero(frame, frame_bytes);

    const uint32_t *pc = chunk->instructions;
    UVMError rc = UVM_OK;

dispatch:
    DISPATCH() {
        CASE(OP_LOADK) {
            frame[uinstr_a(*pc)] = chunk->constants[uinstr_bx(*pc)];
            NEXT();
        }
        CASE(OP_MOVE) {
            frame[uinstr_a(*pc)] = frame[uinstr_b(*pc)];
            NEXT();
        }
        CASE(OP_RET) {
            *out = frame[uinstr_a(*pc)];
            HALT();
        }
        default: {
            /* Unreachable — loader rejects unknown opcodes; opcodes added in
               later tasks plug in here. */
            rc = UVM_TYPE_ERROR;
            HALT();
        }
    }

halt:
    vm->alloc_fn(frame, 0, vm->alloc_ud);
    return rc;
}
