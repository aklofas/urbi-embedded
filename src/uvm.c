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

static UVMError arith_neg(UConst *a, const UConst *b) {
    if (!is_number(b)) return UVM_TYPE_ERROR;
    if (b->kind == UVAL_INT) {
        a->kind = UVAL_INT;
        /* (int64_t)(-(uint64_t)v) wraps INT64_MIN to INT64_MIN.
           Defined behavior; UBSan clean. */
        a->v.i = (int64_t)(-(uint64_t)b->v.i);
        return UVM_OK;
    }
    /* Float negation; IEEE 754 flips the sign bit, defined for NaN/Inf. */
    uconst_set_float(a, -uconst_to_double(b));
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

/* --- Diagnostic infrastructure. --- */

/* Map UValKind to a human-readable name for diagnostic messages. */
static const char *kind_name(uint8_t kind) {
    switch (kind) {
        case UVAL_NIL:   return "Nil";
        case UVAL_INT:   return "Integer";
        case UVAL_FLOAT: return "Float";
        case UVAL_BOOL:  return "Bool";
        case UVAL_STR:   return "String";
    }
    return "unknown";
}

/* Map UOpcode to its mnemonic name for diagnostic messages. */
static const char *op_name(uint8_t op) {
    switch (op) {
        case OP_LOADK: return "OP_LOADK";
        case OP_MOVE:  return "OP_MOVE";
        case OP_ADD:   return "OP_ADD";
        case OP_SUB:   return "OP_SUB";
        case OP_MUL:   return "OP_MUL";
        case OP_DIV:   return "OP_DIV";
        case OP_NEG:   return "OP_NEG";
        case OP_RET:   return "OP_RET";
    }
    return "unknown";
}

/* Fixed-buffer diagnostic writer. Truncates with "..." when the buffer
   fills. Freestanding: no snprintf, no <stdio.h>. */
typedef struct DiagWriter {
    char   *buf;
    size_t  cap;   /* buffer capacity */
    size_t  used;  /* bytes written so far (excluding trailing NUL) */
    bool    truncated;
} DiagWriter;

static void diag_init(DiagWriter *w, char *buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->used = 0;
    w->truncated = false;
    if (cap > 0) buf[0] = '\0';
}

static void diag_write_cstr(DiagWriter *w, const char *s) {
    if (w->truncated) return;
    while (*s) {
        /* Leave 4 bytes for "..." + NUL. */
        if (w->used + 4 >= w->cap) {
            w->truncated = true;
            /* Rewind to make room for ellipsis. */
            size_t ellipsis_pos = (w->cap >= 4) ? w->cap - 4 : 0;
            if (w->used > ellipsis_pos) w->used = ellipsis_pos;
            w->buf[w->used++] = '.';
            w->buf[w->used++] = '.';
            w->buf[w->used++] = '.';
            w->buf[w->used]   = '\0';
            return;
        }
        w->buf[w->used++] = *s++;
    }
    w->buf[w->used] = '\0';
}

/* Write an unsigned integer in decimal. */
static void diag_write_u32(DiagWriter *w, uint32_t n) {
    char tmp[12];
    size_t i = 0;
    if (n == 0) {
        tmp[i++] = '0';
    } else {
        while (n > 0 && i < sizeof(tmp)) {
            tmp[i++] = '0' + (char)(n % 10);
            n /= 10;
        }
    }
    /* Reverse into the writer. */
    while (i > 0) {
        char one[2]; one[0] = tmp[--i]; one[1] = '\0';
        diag_write_cstr(w, one);
    }
}

static void diag_write_size(DiagWriter *w, size_t n) {
    /* size_t is at most 64 bits on our targets; fits in u32 for any
       realistic frame size or pc. Cap for safety. */
    if (n > UINT32_MAX) n = UINT32_MAX;
    diag_write_u32(w, (uint32_t)n);
}

static void diag_write_kind_name(DiagWriter *w, uint8_t kind) {
    diag_write_cstr(w, kind_name(kind));
}

/* Decode source line number for the given PC. Walks line_deltas from
   index 0, summing deltas; abs_lines entries (triggered by INT8_MIN
   sentinel) replace the accumulator. Returns 0 on absent syncline
   data or out-of-range pc. */
static uint32_t vm_line_for_pc(const Chunk *chunk, size_t pc) {
    if (chunk->line_deltas == NULL) return 0;
    if (pc >= chunk->instr_count) return 0;
    uint32_t line = 0;
    size_t abs_idx = 0;
    for (size_t i = 0; i <= pc; i++) {
        int8_t d = chunk->line_deltas[i];
        if (d == INT8_MIN) {
            /* Consult abs_lines; find the entry whose pc matches i. */
            while (abs_idx < chunk->abs_line_count &&
                   chunk->abs_lines[abs_idx].pc < i) {
                abs_idx++;
            }
            if (abs_idx < chunk->abs_line_count &&
                chunk->abs_lines[abs_idx].pc == i) {
                line = chunk->abs_lines[abs_idx].line;
                abs_idx++;
            }
        } else {
            /* Signed add. Cast to int32_t for the intermediate to avoid
               sign-extending an int8_t into a larger unsigned value. */
            line = (uint32_t)((int32_t)line + (int32_t)d);
        }
    }
    return line;
}

/* Format the prefix "source:line:" / "line N:" / "instr N:" into w. */
static void diag_write_prefix(DiagWriter *w, const Chunk *chunk, size_t pc) {
    uint32_t line = vm_line_for_pc(chunk, pc);
    if (line == 0) {
        diag_write_cstr(w, "instr ");
        diag_write_size(w, pc);
        diag_write_cstr(w, ": ");
        return;
    }
    if (chunk->source_name != NULL) {
        diag_write_cstr(w, chunk->source_name);
        diag_write_cstr(w, ":");
    } else {
        diag_write_cstr(w, "line ");
    }
    diag_write_u32(w, line);
    diag_write_cstr(w, ": ");
}

/* Binary-op TypeError: two operand kinds reported.
   Format: "<prefix>TypeError: <OP_NAME> operands must be Integer or Float (got <Kind>, <Kind>)" */
static void vm_format_type_error_binary(UVM *vm, const Chunk *chunk, size_t pc,
                                        uint8_t op, uint8_t b_kind, uint8_t c_kind) {
    DiagWriter w;
    diag_init(&w, vm->last_errmsg, UVM_ERRMSG_CAP);
    diag_write_prefix(&w, chunk, pc);
    diag_write_cstr(&w, "TypeError: ");
    diag_write_cstr(&w, op_name(op));
    diag_write_cstr(&w, " operands must be Integer or Float (got ");
    diag_write_kind_name(&w, b_kind);
    diag_write_cstr(&w, ", ");
    diag_write_kind_name(&w, c_kind);
    diag_write_cstr(&w, ")");
}

/* Unary-op TypeError: one operand kind reported. */
static void vm_format_type_error_unary(UVM *vm, const Chunk *chunk, size_t pc,
                                       uint8_t op, uint8_t b_kind) {
    DiagWriter w;
    diag_init(&w, vm->last_errmsg, UVM_ERRMSG_CAP);
    diag_write_prefix(&w, chunk, pc);
    diag_write_cstr(&w, "TypeError: ");
    diag_write_cstr(&w, op_name(op));
    diag_write_cstr(&w, " operand must be Integer or Float (got ");
    diag_write_kind_name(&w, b_kind);
    diag_write_cstr(&w, ")");
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
    /* Dispatch table keyed by opcode. All 8 M1 opcodes populated;
       loader validates opcode is in [0, OP_MAX) so no unknown-opcode
       guard is needed at this point. */
    static void *dispatch_table[OP_MAX] = {
        [OP_LOADK] = &&label_OP_LOADK,
        [OP_MOVE]  = &&label_OP_MOVE,
        [OP_ADD]   = &&label_OP_ADD,
        [OP_SUB]   = &&label_OP_SUB,
        [OP_MUL]   = &&label_OP_MUL,
        [OP_DIV]   = &&label_OP_DIV,
        [OP_NEG]   = &&label_OP_NEG,
        [OP_RET]   = &&label_OP_RET,
    };
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
                vm_format_type_error_binary(vm, chunk,
                    (size_t)(pc - chunk->instructions),
                    OP_ADD, b->kind, cc->kind);
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
                vm_format_type_error_binary(vm, chunk,
                    (size_t)(pc - chunk->instructions),
                    OP_SUB, b->kind, cc->kind);
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
                vm_format_type_error_binary(vm, chunk,
                    (size_t)(pc - chunk->instructions),
                    OP_MUL, b->kind, cc->kind);
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
                vm_format_type_error_binary(vm, chunk,
                    (size_t)(pc - chunk->instructions),
                    OP_DIV, b->kind, cc->kind);
                HALT();
            }
            NEXT();
        }

        CASE(OP_NEG) {
            UConst *a = &frame[uinstr_a(*pc)];
            const UConst *b = &frame[uinstr_b(*pc)];
            rc = arith_neg(a, b);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm_format_type_error_unary(vm, chunk,
                    (size_t)(pc - chunk->instructions),
                    OP_NEG, b->kind);
                HALT();
            }
            NEXT();
        }

        CASE(OP_RET) {
            *out = frame[uinstr_a(*pc)];
            HALT();
        }

#if !UVM_USE_COMPUTED_GOTO
        default: {
            /* Unreachable — loader rejects unknown opcodes before uvm_run
               is called. The default: branch satisfies -Wswitch-enum. */
            rc = UVM_TYPE_ERROR;
            HALT();
        }
    }
#endif

halt:
    vm->alloc_fn(frame, 0, vm->alloc_ud);
    return rc;
}
