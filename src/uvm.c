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

/* --- Arithmetic helpers.
       Each returns UVM_OK with result written into *a, or UVM_TYPE_ERROR
       leaving *a untouched. Integer overflow uses the unsigned-cast
       trick for portable two's-complement wrap (defined behavior; UBSan
       clean). Float promotion follows LANG-CONVENTIONS §1.3. --- */

/* Convenience: promote an Int/Float UConst to the target Float type. */
static double uconst_to_double(const UConst *v) {
    return v->kind == UVAL_INT ? (double)v->v.i : (double)v->v.f;
}

static void uconst_set_float(UConst *a, const double val) {
    a->kind = UVAL_FLOAT;
#if URBI_FLOAT_TYPE == 8
    a->v.f = val;
#else
    a->v.f = (float)val;
#endif
}

static bool is_number(const UConst *v) {
    return v->kind == UVAL_INT || v->kind == UVAL_FLOAT;
}

static UVMError arith_add(UConst *a, const UConst *b, const UConst *c) {
    if (!is_number(b) || !is_number(c)) return UVM_TYPE_ERROR;
    if (b->kind == UVAL_INT && c->kind == UVAL_INT) {
        a->kind = UVAL_INT;
        a->v.i = (int64_t)((uint64_t)b->v.i + (uint64_t)c->v.i);
        return UVM_OK;
    }
    uconst_set_float(a, uconst_to_double(b) + uconst_to_double(c));
    return UVM_OK;
}

static UVMError arith_mul(UConst *a, const UConst *b, const UConst *c) {
    if (!is_number(b) || !is_number(c)) return UVM_TYPE_ERROR;
    if (b->kind == UVAL_INT && c->kind == UVAL_INT) {
        a->kind = UVAL_INT;
        a->v.i = (int64_t)((uint64_t)b->v.i * (uint64_t)c->v.i);
        return UVM_OK;
    }
    uconst_set_float(a, uconst_to_double(b) * uconst_to_double(c));
    return UVM_OK;
}

static UVMError arith_div(UConst *a, const UConst *b, const UConst *c) {
    if (!is_number(b) || !is_number(c)) return UVM_TYPE_ERROR;
    /* DIV always produces Float per LANG-CONVENTIONS §1.3. IEEE 754
       handles div-by-zero and 0/0 naturally — +Inf for positive/0,
       -Inf for negative/0, NaN for 0/0. */
    uconst_set_float(a, uconst_to_double(b) / uconst_to_double(c));
    return UVM_OK;
}

static UVMError arith_sub(UConst *a, const UConst *b, const UConst *c) {
    if (!is_number(b) || !is_number(c)) return UVM_TYPE_ERROR;
    if (b->kind == UVAL_INT && c->kind == UVAL_INT) {
        a->kind = UVAL_INT;
        a->v.i = (int64_t)((uint64_t)b->v.i - (uint64_t)c->v.i);
        return UVM_OK;
    }
    uconst_set_float(a, uconst_to_double(b) - uconst_to_double(c));
    return UVM_OK;
}

/* --- Local zero-fill. Volatile byte pointer prevents GCC/Clang from
       recognizing the loop and lowering it to a memset libcall under
       -Os, which would break freestanding builds.
       Matches uarena.c's arena_zero pattern precisely. --- */
static void vm_zero(void *const dst, const size_t n) {
    volatile unsigned char *const p = (volatile unsigned char *)dst;
    for (size_t i = 0; i < n; i++) p[i] = 0;
}

/* --- Dispatch macros.
       Under GCC/Clang with computed-goto support (and without
       URBI_VM_FORCE_SWITCH), DISPATCH/CASE/NEXT expand to threaded
       dispatch. Otherwise they expand to switch/case/continue.
       Opcode bodies are written once; both paths use them. --- */

#if !defined(URBI_VM_FORCE_SWITCH) && (defined(__GNUC__) || defined(__clang__))
#  define UVM_USE_COMPUTED_GOTO 1
#else
#  define UVM_USE_COMPUTED_GOTO 0
#endif

#if UVM_USE_COMPUTED_GOTO
#  define DISPATCH()  goto *dispatch_table[uinstr_op(*pc)]
#  define CASE(op)    label_##op:
#  define NEXT()      do { pc++; DISPATCH(); } while (0)
#  define HALT()      goto halt
#else
#  define DISPATCH()  switch (uinstr_op(*pc))
#  define CASE(op)    case (op):
#  define NEXT()      do { pc++; goto dispatch; } while (0)
#  define HALT()      goto halt
#endif

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

#if UVM_USE_COMPUTED_GOTO
    /* Dispatch table keyed by opcode. LOADK, MOVE, ADD, SUB, RET slots are
       populated; MUL/DIV/NEG are NULL and Tasks 9-11 populate them. The
       guard below catches an unimplemented-opcode-as-FIRST-instruction only
       — subsequent NEXT() calls bypass it. Between now and Task 11 this is
       a narrow defensive window; the VM's own test suite only constructs
       chunks using already-implemented opcodes. Task 11 removes the guard
       entirely once all 8 slots are populated, at which point
       loader-validated chunks cannot reach NULL slots. */
    static void *dispatch_table[OP_MAX] = {
        [OP_LOADK] = &&label_OP_LOADK,
        [OP_MOVE]  = &&label_OP_MOVE,
        [OP_ADD]   = &&label_OP_ADD,
        [OP_SUB]   = &&label_OP_SUB,
        [OP_MUL]   = &&label_OP_MUL,
        [OP_DIV]   = &&label_OP_DIV,
        [OP_RET]   = &&label_OP_RET,
        /* OP_NEG added in Task 11 */
    };
    if (dispatch_table[uinstr_op(*pc)] == NULL) goto label_unknown;
    DISPATCH();
#else
dispatch:
    DISPATCH() {
#endif

        CASE(OP_LOADK) {
            frame[uinstr_a(*pc)] = chunk->constants[uinstr_bx(*pc)];
            NEXT();
        }

        CASE(OP_MOVE) {
            frame[uinstr_a(*pc)] = frame[uinstr_b(*pc)];
            NEXT();
        }

        CASE(OP_ADD) {
            UConst *a = &frame[uinstr_a(*pc)];
            const UConst *b = &frame[uinstr_b(*pc)];
            const UConst *cc = &frame[uinstr_c(*pc)];
            rc = arith_add(a, b, cc);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm->last_errmsg[0] = '\0';  /* formatting lands in Task 13 */
                HALT();
            }
            NEXT();
        }

        CASE(OP_SUB) {
            UConst *a = &frame[uinstr_a(*pc)];
            const UConst *b = &frame[uinstr_b(*pc)];
            const UConst *cc = &frame[uinstr_c(*pc)];
            rc = arith_sub(a, b, cc);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm->last_errmsg[0] = '\0';
                HALT();
            }
            NEXT();
        }

        CASE(OP_MUL) {
            UConst *a = &frame[uinstr_a(*pc)];
            const UConst *b = &frame[uinstr_b(*pc)];
            const UConst *cc = &frame[uinstr_c(*pc)];
            rc = arith_mul(a, b, cc);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm->last_errmsg[0] = '\0';
                HALT();
            }
            NEXT();
        }

        CASE(OP_DIV) {
            UConst *a = &frame[uinstr_a(*pc)];
            const UConst *b = &frame[uinstr_b(*pc)];
            const UConst *cc = &frame[uinstr_c(*pc)];
            rc = arith_div(a, b, cc);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm->last_errmsg[0] = '\0';
                HALT();
            }
            NEXT();
        }

        CASE(OP_RET) {
            *out = frame[uinstr_a(*pc)];
            HALT();
        }

#if UVM_USE_COMPUTED_GOTO
        label_unknown:
            rc = UVM_TYPE_ERROR;
            HALT();
#else
        default: {
            /* Unreachable — loader rejects unknown opcodes; opcodes added in
               later tasks plug in here. */
            rc = UVM_TYPE_ERROR;
            HALT();
        }
    }
#endif

halt:
    vm->alloc_fn(frame, 0, vm->alloc_ud);
    return rc;
}
