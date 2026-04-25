/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode interpreter. */

#include "uvm.h"
#include "uintern.h"
#include "uvalue.h"

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
    vm->intern_table = NULL;
    vm->topology_gen = 0u;
    vm->frame_count          = 0;
    vm->open_upvals          = NULL;
    vm->last_return_closure  = NULL;
}

void uvm_destroy(UVM *vm) {
    if (vm == NULL) return;
    uintern_destroy(vm);
    /* Pre-GC: free any closure surviving from the last uvm_run(). */
    if (vm->last_return_closure != NULL && vm->alloc_fn != NULL) {
        vm->alloc_fn(vm->last_return_closure, 0, vm->alloc_ud);
        vm->last_return_closure = NULL;
    }
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

/* Convenience: promote an Int/Float UValue to the target Float type. */
static double uvalue_to_double(const UValue *v) {
    return v->kind == UVAL_INT ? (double)v->v.i : (double)v->v.f;
}

static void uvalue_set_float(UValue *a, const double val) {
    a->kind = UVAL_FLOAT;
#if URBI_FLOAT_TYPE == 8
    a->v.f = val;
#else
    a->v.f = (float)val;
#endif
}

static bool is_number(const UValue *v) {
    return v->kind == UVAL_INT || v->kind == UVAL_FLOAT;
}

static UVMError arith_add(UValue *a, const UValue *b, const UValue *c) {
    if (!is_number(b) || !is_number(c)) return UVM_TYPE_ERROR;
    if (b->kind == UVAL_INT && c->kind == UVAL_INT) {
        a->kind = UVAL_INT;
        a->v.i = (int64_t)((uint64_t)b->v.i + (uint64_t)c->v.i);
        return UVM_OK;
    }
    uvalue_set_float(a, uvalue_to_double(b) + uvalue_to_double(c));
    return UVM_OK;
}

static UVMError arith_sub(UValue *a, const UValue *b, const UValue *c) {
    if (!is_number(b) || !is_number(c)) return UVM_TYPE_ERROR;
    if (b->kind == UVAL_INT && c->kind == UVAL_INT) {
        a->kind = UVAL_INT;
        a->v.i = (int64_t)((uint64_t)b->v.i - (uint64_t)c->v.i);
        return UVM_OK;
    }
    uvalue_set_float(a, uvalue_to_double(b) - uvalue_to_double(c));
    return UVM_OK;
}

static UVMError arith_mul(UValue *a, const UValue *b, const UValue *c) {
    if (!is_number(b) || !is_number(c)) return UVM_TYPE_ERROR;
    if (b->kind == UVAL_INT && c->kind == UVAL_INT) {
        a->kind = UVAL_INT;
        a->v.i = (int64_t)((uint64_t)b->v.i * (uint64_t)c->v.i);
        return UVM_OK;
    }
    uvalue_set_float(a, uvalue_to_double(b) * uvalue_to_double(c));
    return UVM_OK;
}

static UVMError arith_div(UValue *a, const UValue *b, const UValue *c) {
    if (!is_number(b) || !is_number(c)) return UVM_TYPE_ERROR;
    /* DIV always produces Float per LANG-CONVENTIONS §1.3. IEEE 754
       handles div-by-zero and 0/0 naturally — +Inf for positive/0,
       -Inf for negative/0, NaN for 0/0. */
    uvalue_set_float(a, uvalue_to_double(b) / uvalue_to_double(c));
    return UVM_OK;
}

static UVMError arith_neg(UValue *a, const UValue *b) {
    if (!is_number(b)) return UVM_TYPE_ERROR;
    if (b->kind == UVAL_INT) {
        a->kind = UVAL_INT;
        /* (int64_t)(-(uint64_t)v) wraps INT64_MIN to INT64_MIN.
           Defined behavior; UBSan clean. */
        a->v.i = (int64_t)(-(uint64_t)b->v.i);
        return UVM_OK;
    }
    /* Float negation; IEEE 754 flips the sign bit, defined for NaN/Inf. */
    uvalue_set_float(a, -uvalue_to_double(b));
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

/* Map UOpcode to its mnemonic name for diagnostic messages.
   Called only from TypeError/OOM formatting paths, so at M1 the
   OP_LOADK, OP_MOVE, and OP_RET branches are dead (those opcodes
   cannot raise TypeError). Kept as defensive coverage for M2+ when
   new opcodes raise errors and the same formatter is reused. */
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
static uint32_t vm_line_for_pc(const UModule *module, size_t pc) {
    if (module->line_deltas == NULL) return 0;
    if (pc >= module->instr_count) return 0;
    uint32_t line = 0;
    size_t abs_idx = 0;
    for (size_t i = 0; i <= pc; i++) {
        int8_t d = module->line_deltas[i];
        if (d == INT8_MIN) {
            /* Consult abs_lines; find the entry whose pc matches i. */
            while (abs_idx < module->abs_line_count &&
                   module->abs_lines[abs_idx].pc < i) {
                abs_idx++;
            }
            if (abs_idx < module->abs_line_count &&
                module->abs_lines[abs_idx].pc == i) {
                line = module->abs_lines[abs_idx].line;
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
static void diag_write_prefix(DiagWriter *w, const UModule *module, size_t pc) {
    uint32_t line = vm_line_for_pc(module, pc);
    if (line == 0) {
        diag_write_cstr(w, "instr ");
        diag_write_size(w, pc);
        diag_write_cstr(w, ": ");
        return;
    }
    if (module->source_name != NULL) {
        diag_write_cstr(w, module->source_name);
        diag_write_cstr(w, ":");
    } else {
        diag_write_cstr(w, "line ");
    }
    diag_write_u32(w, line);
    diag_write_cstr(w, ": ");
}

/* Binary-op TypeError: two operand kinds reported.
   Format: "<prefix>TypeError: <OP_NAME> operands must be Integer or Float (got <Kind>, <Kind>)" */
static void vm_format_type_error_binary(UVM *vm, const UModule *module, size_t pc,
                                        uint8_t op, uint8_t b_kind, uint8_t c_kind) {
    DiagWriter w;
    diag_init(&w, vm->last_errmsg, UVM_ERRMSG_CAP);
    diag_write_prefix(&w, module, pc);
    diag_write_cstr(&w, "TypeError: ");
    diag_write_cstr(&w, op_name(op));
    diag_write_cstr(&w, " operands must be Integer or Float (got ");
    diag_write_kind_name(&w, b_kind);
    diag_write_cstr(&w, ", ");
    diag_write_kind_name(&w, c_kind);
    diag_write_cstr(&w, ")");
}

/* Unary-op TypeError: one operand kind reported. */
static void vm_format_type_error_unary(UVM *vm, const UModule *module, size_t pc,
                                       uint8_t op, uint8_t b_kind) {
    DiagWriter w;
    diag_init(&w, vm->last_errmsg, UVM_ERRMSG_CAP);
    diag_write_prefix(&w, module, pc);
    diag_write_cstr(&w, "TypeError: ");
    diag_write_cstr(&w, op_name(op));
    diag_write_cstr(&w, " operand must be Integer or Float (got ");
    diag_write_kind_name(&w, b_kind);
    diag_write_cstr(&w, ")");
}

/* Format: "out of memory allocating register frame (<N> bytes requested)" */
static void vm_format_oom(UVM *vm, size_t nbytes) {
    DiagWriter w;
    diag_init(&w, vm->last_errmsg, UVM_ERRMSG_CAP);
    diag_write_cstr(&w, "out of memory allocating register frame (");
    diag_write_size(&w, nbytes);
    diag_write_cstr(&w, " bytes requested)");
}

/* --- Local zero-fill. Volatile byte pointer prevents GCC/Clang from
       recognizing the loop and lowering it to a memset libcall under
       -Os, which would break freestanding builds.
       Matches uarena.c's arena_zero pattern precisely. --- */
static void vm_zero(void *const dst, const size_t n) {
    volatile unsigned char *const p = (volatile unsigned char *)dst;
    for (size_t i = 0; i < n; i++) p[i] = 0;
}

/* --- Closure + upvalue allocation helpers. --- */

/* Allocate a UClosure that can hold `nupvals` upvalue cell pointers.
 * Uses the VM's allocator.  Threads the new closure into *list_head so
 * the caller can free every closure at end-of-run (pre-GC bookkeeping).
 * Returns NULL on OOM. */
static UClosure *vm_alloc_closure(UVM *vm, UProto *proto,
                                  UClosure **list_head) {
    uint8_t nup = proto->nupvals;
    /* sizeof(UClosure) already includes 1 pointer in upvals[1]; add nup-1 more. */
    size_t extra = (nup > 1u) ? (size_t)(nup - 1u) * sizeof(UUpvalCell *) : 0u;
    size_t nbytes = sizeof(UClosure) + extra;
    UClosure *cl = (UClosure *)vm->alloc_fn(NULL, nbytes, vm->alloc_ud);
    if (cl == NULL) return NULL;
    vm_zero(cl, nbytes);
    cl->proto      = proto;
    cl->nupvals    = nup;
    cl->next_alloc = *list_head;
    *list_head     = cl;
    return cl;
}

/* Find or create an open UUpvalCell for &R[slot].
 * Cells are kept in the VM's open_upvals list, sorted by stack address
 * (descending: newest captures at the front). */
static UUpvalCell *vm_open_upvalue(UVM *vm, UValue *slot) {
    /* Scan existing open cells. */
    UUpvalCell *cell = vm->open_upvals;
    while (cell != NULL) {
        if (cell->u.stack_ptr == slot) return cell;
        cell = cell->next;
    }
    /* Create a new open cell. */
    cell = (UUpvalCell *)vm->alloc_fn(NULL, sizeof(UUpvalCell), vm->alloc_ud);
    if (cell == NULL) return NULL;
    vm_zero(cell, sizeof(UUpvalCell));
    cell->on_heap    = false;
    cell->u.stack_ptr = slot;
    cell->next       = vm->open_upvals;
    vm->open_upvals  = cell;
    return cell;
}

/* Heapify all open cells whose stack address is >= threshold.
 * Called by OP_CLOSE and OP_RET (implicitly via frame pop). */
static void vm_close_upvalues(UVM *vm, UValue *threshold) {
    UUpvalCell **link = &vm->open_upvals;
    while (*link != NULL) {
        UUpvalCell *cell = *link;
        if (cell->u.stack_ptr >= threshold) {
            cell->u.value = *cell->u.stack_ptr;
            cell->on_heap  = true;
            *link = cell->next;
            cell->next = NULL;
        } else {
            link = &cell->next;
        }
    }
}

/* Free all open upvalue cells (called when VM is destroyed or rerun).
 * Closed cells are owned by UClosure objects; open cells are VM-owned. */
static void vm_free_open_upvalues(UVM *vm) {
    UUpvalCell *cell = vm->open_upvals;
    while (cell != NULL) {
        UUpvalCell *next = cell->next;
        vm->alloc_fn(cell, 0, vm->alloc_ud);
        cell = next;
    }
    vm->open_upvals = NULL;
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

/* Generic unsupported-opcode error message.  Used by M2 placeholder arms
 * that will be replaced by real implementations in later tasks. */
static void vm_format_type_error_msg(UVM *vm, const char *msg) {
    DiagWriter w;
    diag_init(&w, vm->last_errmsg, UVM_ERRMSG_CAP);
    diag_write_cstr(&w, "TypeError: ");
    diag_write_cstr(&w, msg);
}

/* --- uvm_run --- */

UVMError uvm_run(UVM *vm, const UModule *module, UValue *out) {
    /* Reset error state at entry so callers who run multiple modules
       don't see stale last_error from a prior failure. */
    vm->last_error = UVM_OK;
    vm->last_errmsg[0] = '\0';
    vm->frame_count = 0;
    vm_free_open_upvalues(vm);

    /* Pre-GC: free the closure returned by the previous uvm_run (if any).
     * The caller had one run's lifetime to inspect it. */
    if (vm->last_return_closure != NULL) {
        UClosure *prev = vm->last_return_closure;
        uint8_t nup = prev->nupvals;
        size_t extra = (nup > 1u) ? (size_t)(nup - 1u) * sizeof(UUpvalCell *) : 0u;
        (void)extra;
        vm->alloc_fn(prev, 0, vm->alloc_ud);
        vm->last_return_closure = NULL;
    }

    /* Initialize out to Nil; overwritten on OP_RET success. */
    UValue nil = {0};  /* kind = UVAL_NIL, payload zeroed */
    *out = nil;

    /* Empty module: no instructions to dispatch; return Nil. */
    if (module->instr_count == 0) {
        return UVM_OK;
    }

    /* Allocate a unified register stack for all frames.
       UVM_STACK_CAP slots; zero-initialized so every register starts Nil. */
    const size_t stack_bytes = UVM_STACK_CAP * sizeof(UValue);
    UValue *stack = (UValue *)vm->alloc_fn(NULL, stack_bytes, vm->alloc_ud);
    if (stack == NULL) {
        vm->last_error = UVM_OOM;
        vm_format_oom(vm, stack_bytes);
        return UVM_OOM;
    }
    vm_zero(stack, stack_bytes);

    /* Current-frame state: register base, instruction pointer, proto. */
    UValue *R  = stack;         /* current frame's register base */
    const uint32_t *pc = module->instructions;
    const UValue   *cur_consts = module->constants;
    UVMError rc = UVM_OK;

    /* Pre-GC closure list: every UClosure allocated this run is threaded
     * here via next_alloc; freed at halt (after *out is copied out). */
    UClosure *closure_list = NULL;

    /* Saved PC base for the current frame — used to compute pc offsets
     * for diagnostic messages.  Updated on every frame push/pop. */
    const uint32_t *pc_base = module->instructions;

#if UVM_USE_COMPUTED_GOTO
    /* Dispatch table keyed by opcode. All 8 M1 opcodes populated;
       loader validates opcode is in [0, OP_MAX) so no unknown-opcode
       guard is needed at this point. */
    static void *dispatch_table[OP_MAX] = {
        [OP_LOADK]      = &&label_OP_LOADK,
        [OP_MOVE]       = &&label_OP_MOVE,
        [OP_ADD]        = &&label_OP_ADD,
        [OP_SUB]        = &&label_OP_SUB,
        [OP_MUL]        = &&label_OP_MUL,
        [OP_DIV]        = &&label_OP_DIV,
        [OP_NEG]        = &&label_OP_NEG,
        [OP_RET]        = &&label_OP_RET,
        [OP_LOADNIL]    = &&label_OP_LOADNIL,
        [OP_LOADBOOL]   = &&label_OP_LOADBOOL,
        [OP_LOADVOID]   = &&label_OP_LOADVOID,
        [OP_GETUPVAL]   = &&label_OP_GETUPVAL,
        [OP_SETUPVAL]   = &&label_OP_SETUPVAL,
        [OP_CLOSURE]    = &&label_OP_CLOSURE,
        [OP_CLOSE]      = &&label_OP_CLOSE,
        [OP_CALL]       = &&label_OP_CALL,
        [OP_JMP]        = &&label_OP_JMP,
        [OP_TEST]       = &&label_OP_TEST,
        [OP_TESTSET]    = &&label_OP_TESTSET,
        [OP_EQ]         = &&label_OP_EQ,
        [OP_NEQ]        = &&label_OP_NEQ,
        [OP_LT]         = &&label_OP_LT,
        [OP_LE]         = &&label_OP_LE,
        [OP_YIELD]      = &&label_OP_YIELD,
        [OP_FORK_DETACH]= &&label_OP_FORK_DETACH,
        [OP_FORK_JOIN]  = &&label_OP_FORK_JOIN,
        [OP_JOIN_WAIT]  = &&label_OP_JOIN_WAIT,
        [OP_GETSLOT]    = &&label_OP_GETSLOT,
        [OP_SETSLOT]    = &&label_OP_SETSLOT,
    };

    DISPATCH();
#else
dispatch:
    DISPATCH() {
#endif

        CASE(OP_LOADK) {
            R[uinstr_a(*pc)] = cur_consts[uinstr_bx(*pc)];
            NEXT();
        }

        CASE(OP_MOVE) {
            R[uinstr_a(*pc)] = R[uinstr_b(*pc)];
            NEXT();
        }

        CASE(OP_ADD) {
            UValue *a = &R[uinstr_a(*pc)];
            const UValue *b = &R[uinstr_b(*pc)];
            const UValue *cc = &R[uinstr_c(*pc)];
            rc = arith_add(a, b, cc);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm_format_type_error_binary(vm, module,
                    (size_t)(pc - pc_base),
                    OP_ADD, b->kind, cc->kind);
                HALT();
            }
            NEXT();
        }

        CASE(OP_SUB) {
            UValue *a = &R[uinstr_a(*pc)];
            const UValue *b = &R[uinstr_b(*pc)];
            const UValue *cc = &R[uinstr_c(*pc)];
            rc = arith_sub(a, b, cc);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm_format_type_error_binary(vm, module,
                    (size_t)(pc - pc_base),
                    OP_SUB, b->kind, cc->kind);
                HALT();
            }
            NEXT();
        }

        CASE(OP_MUL) {
            UValue *a = &R[uinstr_a(*pc)];
            const UValue *b = &R[uinstr_b(*pc)];
            const UValue *cc = &R[uinstr_c(*pc)];
            rc = arith_mul(a, b, cc);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm_format_type_error_binary(vm, module,
                    (size_t)(pc - pc_base),
                    OP_MUL, b->kind, cc->kind);
                HALT();
            }
            NEXT();
        }

        CASE(OP_DIV) {
            UValue *a = &R[uinstr_a(*pc)];
            const UValue *b = &R[uinstr_b(*pc)];
            const UValue *cc = &R[uinstr_c(*pc)];
            rc = arith_div(a, b, cc);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm_format_type_error_binary(vm, module,
                    (size_t)(pc - pc_base),
                    OP_DIV, b->kind, cc->kind);
                HALT();
            }
            NEXT();
        }

        CASE(OP_NEG) {
            UValue *a = &R[uinstr_a(*pc)];
            const UValue *b = &R[uinstr_b(*pc)];
            rc = arith_neg(a, b);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm_format_type_error_unary(vm, module,
                    (size_t)(pc - pc_base),
                    OP_NEG, b->kind);
                HALT();
            }
            NEXT();
        }

        CASE(OP_RET) {
            UValue retval = R[uinstr_a(*pc)];

            if (vm->frame_count == 0) {
                /* Top-level — exit uvm_run. */
                *out = retval;
                HALT();
            }

            /* Pop the call frame. */
            UCallFrame *done = &vm->frames[--vm->frame_count];

            /* Close any open upvalues that point into this frame's registers.
             * Threshold = done->base + 1 (first slot of callee's frame was
             * done->base[result_dest_reg + 1], i.e. R_caller[a+1]). */
            vm_close_upvalues(vm, done->base + done->result_dest_reg + 1);

            /* Restore caller's register window and instruction pointer. */
            R     = done->base;
            pc    = done->pc + 1;  /* advance past the OP_CALL */
            pc_base    = module->instructions;  /* diagnostic; approximate */
            cur_consts = (vm->frame_count > 0 &&
                          vm->frames[vm->frame_count - 1].closure != NULL)
                         ? vm->frames[vm->frame_count - 1].closure->proto->constants
                         : module->constants;

            /* Write return value into caller's result slot R[a]. */
            R[done->result_dest_reg] = retval;

            DISPATCH();
#if !UVM_USE_COMPUTED_GOTO
            break; /* suppress -Wimplicit-fallthrough in switch mode */
#endif
        }

        CASE(OP_LOADNIL) {
            R[uinstr_a(*pc)].kind = (uint8_t)UVAL_NIL;
            NEXT();
        }

        CASE(OP_LOADBOOL) {
            R[uinstr_a(*pc)].kind  = (uint8_t)UVAL_BOOL;
            R[uinstr_a(*pc)].v.i   = uinstr_b(*pc) != 0 ? 1 : 0;
            if (uinstr_c(*pc)) { pc++; }
            NEXT();
        }

        CASE(OP_LOADVOID) {
            R[uinstr_a(*pc)].kind = (uint8_t)UVAL_VOID;
            NEXT();
        }

        CASE(OP_GETUPVAL) {
            /* ABC: R[A] := upvalue[B] from the current frame's closure. */
            UClosure *cur_cl = (vm->frame_count > 0)
                             ? vm->frames[vm->frame_count - 1].closure
                             : NULL;
            if (cur_cl == NULL) {
                rc = UVM_TYPE_ERROR;
                vm->last_error = rc;
                vm_format_type_error_msg(vm, "GETUPVAL: no closure in current frame");
                HALT();
            }
            {
                uint8_t b = uinstr_b(*pc);
                UUpvalCell *uvc = cur_cl->upvals[b];
                R[uinstr_a(*pc)] = uvc->on_heap ? uvc->u.value
                                                    : *uvc->u.stack_ptr;
            }
            NEXT();
        }

        CASE(OP_SETUPVAL) {
            /* ABC: upvalue[B] := R[A] for the current frame's closure. */
            UClosure *cur_cl = (vm->frame_count > 0)
                             ? vm->frames[vm->frame_count - 1].closure
                             : NULL;
            if (cur_cl == NULL) {
                rc = UVM_TYPE_ERROR;
                vm->last_error = rc;
                vm_format_type_error_msg(vm, "SETUPVAL: no closure in current frame");
                HALT();
            }
            {
                uint8_t a = uinstr_a(*pc);
                uint8_t b = uinstr_b(*pc);
                UUpvalCell *uvc = cur_cl->upvals[b];
                if (uvc->on_heap) {
                    uvc->u.value = R[a];
                } else {
                    *uvc->u.stack_ptr = R[a];
                }
            }
            NEXT();
        }

        CASE(OP_CLOSURE) {
            /* ABx: R[A] := new closure from module->nested[Bx].
             * Reads nupvals pseudo-instructions (OP_MOVE-encoded) immediately
             * after, each specifying (B=in_stack, C=src_idx). */
            uint8_t  a  = uinstr_a(*pc);
            uint16_t bx = uinstr_bx(*pc);
            if ((size_t)bx >= module->nested_count) {
                rc = UVM_TYPE_ERROR;
                vm->last_error = rc;
                vm_format_type_error_msg(vm, "CLOSURE: proto index out of range");
                HALT();
            }
            UProto *child_proto = module->nested[bx];
            UClosure *cl = vm_alloc_closure(vm, child_proto, &closure_list);
            if (cl == NULL) {
                rc = UVM_OOM;
                vm->last_error = rc;
                vm_format_oom(vm, sizeof(UClosure));
                HALT();
            }
            /* Read nupvals pseudo-instructions. */
            {
                int i;
                for (i = 0; i < (int)child_proto->nupvals; i++) {
                    pc++;
                    uint8_t in_stack = uinstr_b(*pc);
                    uint8_t src_idx  = uinstr_c(*pc);
                    if (in_stack) {
                        UUpvalCell *uvc = vm_open_upvalue(vm, &R[src_idx]);
                        if (uvc == NULL) {
                            vm->alloc_fn(cl, 0, vm->alloc_ud);
                            rc = UVM_OOM;
                            vm->last_error = rc;
                            vm_format_oom(vm, sizeof(UUpvalCell));
                            HALT();
                        }
                        cl->upvals[i] = uvc;
                    } else {
                        /* Re-capture: copy parent closure's upvalue pointer. */
                        UClosure *par_cl = (vm->frame_count > 0)
                                         ? vm->frames[vm->frame_count - 1].closure
                                         : NULL;
                        if (par_cl == NULL || src_idx >= par_cl->nupvals) {
                            vm->alloc_fn(cl, 0, vm->alloc_ud);
                            rc = UVM_TYPE_ERROR;
                            vm->last_error = rc;
                            vm_format_type_error_msg(vm, "CLOSURE: upvalue re-capture out of range");
                            HALT();
                        }
                        cl->upvals[i] = par_cl->upvals[src_idx];
                    }
                }
            }
            R[a].kind  = (uint8_t)UVAL_CLOSURE;
            R[a].v.p   = cl;
            NEXT();
        }

        CASE(OP_CLOSE) {
            /* ABC: heapify all open upvalue cells at R >= R[A]. */
            vm_close_upvalues(vm, &R[uinstr_a(*pc)]);
            NEXT();
        }

        CASE(OP_CALL) {
            /* ABC: R[A](R[A+1]..R[A+B-1]); B-1 = nargs, C unused at T14.
             * After the call, the result overwrites R[A]. */
            uint8_t a = uinstr_a(*pc);
            uint8_t b = uinstr_b(*pc);
            int nargs = (int)b - 1;

            if (R[a].kind != (uint8_t)UVAL_CLOSURE) {
                rc = UVM_TYPE_ERROR;
                vm->last_error = rc;
                vm_format_type_error_msg(vm, "CALL: callee is not a closure");
                HALT();
            }
            UClosure *callee = (UClosure *)R[a].v.p;
            if (nargs != (int)callee->proto->nparams) {
                rc = UVM_TYPE_ERROR;
                vm->last_error = rc;
                vm_format_type_error_msg(vm, "CALL: wrong argument count");
                HALT();
            }
            if (vm->frame_count >= UVM_MAX_FRAMES) {
                rc = UVM_TYPE_ERROR;
                vm->last_error = rc;
                vm_format_type_error_msg(vm, "CALL: call stack overflow");
                HALT();
            }

            /* Check stack space: callee's frame starts at R[a+1]. */
            if ((R + a + 1 + callee->proto->max_reg + 1) > (stack + UVM_STACK_CAP)) {
                rc = UVM_OOM;
                vm->last_error = rc;
                vm_format_oom(vm, (size_t)(callee->proto->max_reg + 1) * sizeof(UValue));
                HALT();
            }

            /* Push a new frame record.  This frame record stores what to restore
             * on OP_RET, plus the callee's closure (for GETUPVAL during the call). */
            UCallFrame *new_frame = &vm->frames[vm->frame_count++];
            new_frame->closure         = callee;   /* executing closure */
            new_frame->proto           = callee->proto;
            new_frame->pc              = pc;       /* points AT OP_CALL in caller */
            new_frame->base            = R;        /* caller's register base */
            new_frame->result_dest_reg = (int)a;  /* where to write result */

            /* Switch to callee frame. Args R[a+1..a+nargs] become R[0..nargs-1]. */
            R     = &R[a + 1];
            pc    = callee->proto->instructions;
            pc_base    = pc;
            cur_consts = callee->proto->constants ? callee->proto->constants
                                                  : module->constants;

            /* Zero registers beyond nparams up to max_reg. */
            {
                int si;
                for (si = nargs; si <= (int)callee->proto->max_reg; si++) {
                    UValue z = {0};
                    R[si] = z;
                }
            }

            DISPATCH();
#if !UVM_USE_COMPUTED_GOTO
            break; /* suppress -Wimplicit-fallthrough in switch mode */
#endif
        }

        CASE(OP_JMP) {
            /* ABx: pc += signed(Bx) - 32768.  Offset is applied after the
             * normal pc++ in NEXT, so we pre-adjust by (offset - 1). */
            int offset = (int)uinstr_bx(*pc) - 32768;
            pc += offset;
            NEXT();
        }

        CASE(OP_TEST) {
            /* ABC: if (truthy(R[A]) == C) pc++ (skip next instr) */
            const UValue *a = &R[uinstr_a(*pc)];
            bool truthy = uvalue_truthy(a);
            if ((int)truthy == (int)uinstr_c(*pc)) { pc++; }
            NEXT();
        }

        CASE(OP_TESTSET) {
            /* ABC: if (truthy(R[B]) == C) pc++ else R[A] := R[B] */
            const UValue *b = &R[uinstr_b(*pc)];
            bool truthy = uvalue_truthy(b);
            if ((int)truthy == (int)uinstr_c(*pc)) {
                pc++;
            } else {
                R[uinstr_a(*pc)] = *b;
            }
            NEXT();
        }

        CASE(OP_EQ) {
            /* ABC: if ((R[B]==R[C]) != A) pc++ */
            const UValue *b = &R[uinstr_b(*pc)];
            const UValue *c = &R[uinstr_c(*pc)];
            bool eq = uvalue_equal(b, c);
            if ((int)eq != (int)uinstr_a(*pc)) { pc++; }
            NEXT();
        }

        CASE(OP_NEQ) {
            /* ABC: if ((R[B]!=R[C]) != A) pc++ — emitter normalises NEQ to
               OP_EQ with a_bit=0; this arm handles any residual direct use. */
            const UValue *b = &R[uinstr_b(*pc)];
            const UValue *c = &R[uinstr_c(*pc)];
            bool neq = !uvalue_equal(b, c);
            if ((int)neq != (int)uinstr_a(*pc)) { pc++; }
            NEXT();
        }

        CASE(OP_LT) {
            /* ABC: if ((R[B]<R[C]) != A) pc++ — numeric only at M2 */
            const UValue *b = &R[uinstr_b(*pc)];
            const UValue *c = &R[uinstr_c(*pc)];
            bool lt = false;
            if (uvalue_lt(b, c, &lt) != UVAL_CMP_OK) {
                rc = UVM_TYPE_ERROR;
                vm->last_error = rc;
                vm_format_type_error_binary(vm, module,
                    (size_t)(pc - pc_base), OP_LT, b->kind, c->kind);
                HALT();
            }
            if ((int)lt != (int)uinstr_a(*pc)) { pc++; }
            NEXT();
        }

        CASE(OP_LE) {
            /* ABC: if ((R[B]<=R[C]) != A) pc++ — numeric only at M2 */
            const UValue *b = &R[uinstr_b(*pc)];
            const UValue *c = &R[uinstr_c(*pc)];
            bool le = false;
            if (uvalue_le(b, c, &le) != UVAL_CMP_OK) {
                rc = UVM_TYPE_ERROR;
                vm->last_error = rc;
                vm_format_type_error_binary(vm, module,
                    (size_t)(pc - pc_base), OP_LE, b->kind, c->kind);
                HALT();
            }
            if ((int)le != (int)uinstr_a(*pc)) { pc++; }
            NEXT();
        }

        CASE(OP_YIELD) {
            /* M3 scheduler yield; no-op at M2. */
            NEXT();
        }

        CASE(OP_FORK_DETACH) {
            /* M3 `,` separator runtime. Structural placeholder. */
            rc = UVM_TYPE_ERROR;
            vm->last_error = rc;
            vm_format_type_error_msg(vm, "FORK_DETACH: not implemented until M3");
            HALT();
        }

        CASE(OP_FORK_JOIN) {
            /* M3 `&` separator runtime. Structural placeholder. */
            rc = UVM_TYPE_ERROR;
            vm->last_error = rc;
            vm_format_type_error_msg(vm, "FORK_JOIN: not implemented until M3");
            HALT();
        }

        CASE(OP_JOIN_WAIT) {
            /* M3 `&` join-point. Structural placeholder. */
            rc = UVM_TYPE_ERROR;
            vm->last_error = rc;
            vm_format_type_error_msg(vm, "JOIN_WAIT: not implemented until M3");
            HALT();
        }

        CASE(OP_GETSLOT) {
            /* M4 slot read. Structural placeholder. */
            rc = UVM_TYPE_ERROR;
            vm->last_error = rc;
            vm_format_type_error_msg(vm, "GETSLOT: not implemented until M4");
            HALT();
        }

        CASE(OP_SETSLOT) {
            /* M4 slot write. Structural placeholder. */
            rc = UVM_TYPE_ERROR;
            vm->last_error = rc;
            vm_format_type_error_msg(vm, "SETSLOT: not implemented until M4");
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
    vm->alloc_fn(stack, 0, vm->alloc_ud);
    vm->frame_count = 0;

    /* Pre-GC: free every closure allocated this run, except the one
     * returned to the caller via *out.  That closure is kept alive in
     * vm->last_return_closure until the next uvm_run() or uvm_destroy(). */
    {
        UClosure *out_cl = (out->kind == (uint8_t)UVAL_CLOSURE)
                           ? (UClosure *)out->v.p : NULL;
        vm->last_return_closure = out_cl;

        UClosure *cl = closure_list;
        while (cl != NULL) {
            UClosure *next = cl->next_alloc;
            if (cl != out_cl) {
                vm->alloc_fn(cl, 0, vm->alloc_ud);
            }
            cl = next;
        }
    }
    return rc;
}
