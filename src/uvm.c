/* SPDX-License-Identifier: BSD-3-Clause */
/* Bytecode interpreter. */

/* _POSIX_C_SOURCE must be defined before any system header; guard against
   re-definition in case the build system already defines it. */
#if __STDC_HOSTED__ && (defined(__linux__) || defined(__APPLE__) || defined(__unix__))
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  define UVM_HAVE_CLOCK_GETTIME 1
#endif

#include "uvm.h"
#include "urbi.h"    /* URBI_CALLBACK_WARN_US, URBI_WATCHDOG_WARN */
#include "ustrand.h"
#include "uintern.h"
#include "uvalue.h"
#include "usched_cooperative.h"
#include "uvm_internal.h"
#include "uunwind.h"
#include "urealm.h"
#include "uevent_ring.h"
#include "m3_forward_decls.h"
#include "uhandle.h" /* host_handle_walk_roots (T27) */

#if __STDC_HOSTED__
#  include <stdlib.h>
#  if defined(UVM_HAVE_CLOCK_GETTIME)
#    include <time.h>
#  endif
#endif  /* __STDC_HOSTED__ */

/* Default allocator: realloc semantics. Only compiled in hosted builds. */
#if __STDC_HOSTED__
static void *uvm_stdlib_realloc(void *ptr, size_t nbytes, void *ud) {
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return realloc(ptr, nbytes);
}
#endif

/* --- Default host time source ---
   Returns monotonic microseconds on POSIX hosts (Linux/macOS/BSD); returns 0
   on freestanding targets and non-POSIX hosted targets.
   Embedded callers MUST override via the host_time_us field after uvm_init(). */
static uint64_t default_host_time_us_stub(void) {
#if defined(UVM_HAVE_CLOCK_GETTIME)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#else
    /* Freestanding or non-POSIX hosted: no clock without platform BSP. */
    return 0u;
#endif
}

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
    vm->last_return_closure  = NULL;

    /* --- M3 field zero-init (rows 8, 9, 10, 11) --- */
    /* 5-flag liveness counters (Rule X). */
    vm->strand_runnable_count   = 0u;
    vm->strand_suspended_count  = 0u;
    vm->watcher_active_count    = 0u;
    vm->event_queue_count       = 0u;
    vm->wakeup_pending_count    = 0u;
    vm->host_call_pending_count = 0u;

    /* Realm / fatal-strand pointers. */
    vm->realms_head  = NULL;
    vm->global_realm = NULL;
    vm->fatal_strand = NULL;

    /* Scheduler queues and step-driver state. */
    vm->ready_head             = NULL;
    vm->ready_tail             = NULL;
    vm->sleep_q_head           = NULL;
    vm->step_budget_remaining  = 0u;
    vm->gc_pending             = 0u;
    vm->watcher_dirty_count    = 0u;
    vm->flag_preemption        = 0u;
    vm->flag_reserved[0]       = 0u;
    vm->flag_reserved[1]       = 0u;
    vm->flag_reserved[2]       = 0u;

    /* ISR ring: allocate and initialise the SPSC event ring. */
    vm->event_ring = NULL;
    if (vm->alloc_fn) {
        vm->event_ring = (struct UEventRing *)vm->alloc_fn(
                NULL, sizeof(UEventRing), vm->alloc_ud);
        if (vm->event_ring) {
            uevent_ring_init(vm->event_ring);
        }
        /* OOM: leave event_ring NULL; inject/drain guard against it. */
    }

    /* GC state machine.
     * gc_phase = 0 = IDLE per row 10 §6.2; named constant lands at T22. */
    vm->gc_phase            = 0u;
    vm->current_white       = 0u;
    vm->gc_paused           = 0u;
    vm->in_destroy_callback = 0u;
    vm->gc_live_bytes       = 0u;
    vm->gc_total_allocated  = 0u;
    vm->all_cells_head      = NULL;
    vm->gray_work_head      = NULL;
    vm->sweep_cursor        = NULL;
    vm->sweep_cursor_prev   = NULL;

    /* T19 ISR-check + debug watchdog hooks: initialize before any subsystem
     * calls URBI_ASSERT_NOT_ISR (T23 onwards). */
    vm->isr_check_fn           = NULL;
    vm->host_log_fn            = NULL;
    vm->callback_warn_us       = URBI_CALLBACK_WARN_US;
    vm->callback_watchdog_mode = URBI_WATCHDOG_WARN;
    vm->pad_watchdog[0]        = 0u;
    vm->pad_watchdog[1]        = 0u;
    vm->pad_watchdog[2]        = 0u;

    /* Delegate threshold + debt init to the GC strategy (T23). */
    urbi_gc_init(vm);

    /* GC root provider registry: zero before registering providers. */
    {
        uint8_t i;
        for (i = 0u; i < (uint8_t)URBI_MAX_ROOT_PROVIDERS; i++) {
            vm->root_providers[i] = NULL;
        }
    }
    vm->root_provider_count = 0u;

    /* Register default root providers (T26).
     * Order: scheduler first, then realm, intern, host-handle.
     * T36 adds: urbi_gc_register_root_provider(vm, watcher_table_walk_roots). */
    urbi_gc_register_root_provider(vm, sched_walk_roots);
    urbi_gc_register_root_provider(vm, realm_list_walk_roots);
    urbi_gc_register_root_provider(vm, intern_table_walk_roots);
    urbi_gc_register_root_provider(vm, host_handle_walk_roots);

    /* Type table + host-handle table. */
    {
        int i;
        for (i = 0; i < 256; i++) {
            vm->type_table[i] = NULL;
        }
    }
    vm->host_type_count      = 0u;
    vm->handle_table         = NULL;
    vm->handle_table_cap     = 0u;
    vm->handle_table_next_id = 0u;

    /* Watcher pool. */
    vm->watcher_pool_base      = NULL;
    vm->watcher_pool_freelist  = NULL;
    vm->active_watchers_head   = NULL;
    vm->watcher_pool_in_use    = 0u;
    vm->watcher_pool_high_water = 0u;
    vm->in_watcher_eval        = 0u;
    vm->pad_in_eval[0]         = 0u;
    vm->pad_in_eval[1]         = 0u;
    vm->pad_in_eval[2]         = 0u;
    vm->watcher_scratch_frame  = NULL;
    vm->pending_onleave_head   = NULL;
    vm->pending_onleave_tail   = NULL;

    /* Host time hook: default stub; embedded callers override post-init. */
    vm->host_time_us = default_host_time_us_stub;
}

void uvm_destroy(UVM *vm) {
    if (vm == NULL) return;

    /* --- M3 teardown stubs (in reverse-init order) ---
     * Subsystem-owned teardowns are deferred to their landing tasks. */
    urealm_teardown_all(vm);  /* T14: destroy all live Realms */
    /* T32: uwatcher_pool_destroy(vm); */
    /* GC destroy must run after all subsystems that hold GC-managed cells.
     * Realm teardown (above) releases bindings; remaining infrastructure (event ring,
     * sched queues) is freed below — none of it owns GC cells. */
    urbi_gc_destroy(vm);      /* T23: free all GC-managed cells */
    if (vm->event_ring && vm->alloc_fn) {
        vm->alloc_fn(vm->event_ring, 0, vm->alloc_ud);
        vm->event_ring = NULL;
    }

    /* Free any M3 heap fields that T4 itself allocated (none at T4, but
     * handle_table and watcher_scratch_frame may be set by callers; free
     * them via the VM allocator so freestanding builds use the right hook). */
    if (vm->handle_table != NULL && vm->alloc_fn != NULL) {
        vm->alloc_fn(vm->handle_table, 0, vm->alloc_ud);
        vm->handle_table = NULL;
    }
    if (vm->watcher_scratch_frame != NULL && vm->alloc_fn != NULL) {
        vm->alloc_fn(vm->watcher_scratch_frame, 0, vm->alloc_ud);
        vm->watcher_scratch_frame = NULL;
    }

    /* M2 baseline teardown. */
    uintern_destroy(vm);
    /* Pre-GC: free any closure surviving from the last uvm_run(). */
    if (vm->last_return_closure != NULL && vm->alloc_fn != NULL) {
        vm->alloc_fn(vm->last_return_closure, 0, vm->alloc_ud);
        vm->last_return_closure = NULL;
    }
    /* Note: open_upvals is now on the strand, not the VM.
       The uvm_run adapter cleans up strand.open_upvals before destroy. */
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

/* Map UOpcode to its mnemonic name for diagnostic messages. */
static const char *op_name(uint8_t op) {
    switch (op) {
        case OP_LOADK:          return "OP_LOADK";
        case OP_MOVE:           return "OP_MOVE";
        case OP_ADD:            return "OP_ADD";
        case OP_SUB:            return "OP_SUB";
        case OP_MUL:            return "OP_MUL";
        case OP_DIV:            return "OP_DIV";
        case OP_NEG:            return "OP_NEG";
        case OP_RET:            return "OP_RET";
        case OP_LOADNIL:        return "OP_LOADNIL";
        case OP_LOADBOOL:       return "OP_LOADBOOL";
        case OP_LOADVOID:       return "OP_LOADVOID";
        case OP_GETUPVAL:       return "OP_GETUPVAL";
        case OP_SETUPVAL:       return "OP_SETUPVAL";
        case OP_CLOSURE:        return "OP_CLOSURE";
        case OP_CLOSE:          return "OP_CLOSE";
        case OP_CALL:           return "OP_CALL";
        case OP_JMP:            return "OP_JMP";
        case OP_TEST:           return "OP_TEST";
        case OP_TESTSET:        return "OP_TESTSET";
        case OP_EQ:             return "OP_EQ";
        case OP_NEQ:            return "OP_NEQ";
        case OP_LT:             return "OP_LT";
        case OP_LE:             return "OP_LE";
        case OP_YIELD:          return "OP_YIELD";
        case OP_FORK_DETACH:    return "OP_FORK_DETACH";
        case OP_FORK_JOIN:      return "OP_FORK_JOIN";
        case OP_JOIN_WAIT:      return "OP_JOIN_WAIT";
        case OP_GETSLOT:        return "OP_GETSLOT";
        case OP_SETSLOT:        return "OP_SETSLOT";
        /* M3 row 7 control-transfer opcodes */
        case OP_THROW:          return "OP_THROW";
        case OP_TAG_STOP:       return "OP_TAG_STOP";
        case OP_TRY_BEGIN:      return "OP_TRY_BEGIN";
        case OP_TRY_END:        return "OP_TRY_END";
        case OP_PUSH_TAG:       return "OP_PUSH_TAG";
        case OP_POP_TAG:        return "OP_POP_TAG";
        case OP_PUSH_FRAME_GUARD:     return "OP_PUSH_FRAME_GUARD";
        case OP_RESUME:               return "OP_RESUME";
        case OP_LOAD_CATCH_VALUE:     return "OP_LOAD_CATCH_VALUE";
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
 * Cells are kept in the strand's open_upvals list, sorted by stack address
 * (descending: newest captures at the front). */
static UUpvalCell *vm_open_upvalue(UVM *vm, UStrand *s, UValue *slot) {
    /* Scan existing open cells. */
    UUpvalCell *cell = s->open_upvals;
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
    cell->next       = s->open_upvals;
    s->open_upvals   = cell;
    return cell;
}

/* Heapify all open cells whose stack address is >= threshold.
 * Removed cells are appended to *closed_list (for per-run bulk free at halt).
 * Called by OP_CLOSE, OP_RET, and urbi_unwind.
 * Declared non-static (exported via uvm_internal.h) for uunwind.c access. */
void vm_close_upvalues(UStrand *s, UValue *threshold,
                       UUpvalCell **closed_list) {
    UUpvalCell **link = &s->open_upvals;
    while (*link != NULL) {
        UUpvalCell *cell = *link;
        if (cell->u.stack_ptr >= threshold) {
            cell->u.value = *cell->u.stack_ptr;
            cell->on_heap  = true;
            *link = cell->next;
            /* Thread into closed_list using the now-free next pointer. */
            cell->next = *closed_list;
            *closed_list = cell;
        } else {
            link = &cell->next;
        }
    }
}

/* Free all open upvalue cells remaining on a strand. */
static void vm_free_open_upvalues(UVM *vm, UStrand *s) {
    UUpvalCell *cell = s->open_upvals;
    while (cell != NULL) {
        UUpvalCell *next = cell->next;
        vm->alloc_fn(cell, 0, vm->alloc_ud);
        cell = next;
    }
    s->open_upvals = NULL;
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
#  define DISPATCH()  goto *dispatch_table[uinstr_op(*s->pc)]
#  define CASE(op)    label_##op:
#  define NEXT()      do { s->pc++; DISPATCH(); } while (0)
#  define HALT()      goto halt_error
#else
#  define DISPATCH()  switch (uinstr_op(*s->pc))
#  define CASE(op)    case (op):
#  define NEXT()      do { s->pc++; goto dispatch; } while (0)
#  define HALT()      goto halt_error
#endif

/* Dispatch-time assertion for placeholder opcode stubs.
 * In hosted builds (tests, REPL) this triggers assert() so CI catches
 * stray row-7 opcodes emitted without an emit path.  In freestanding
 * builds it is a no-op — the stub immediately sets strand DEAD, which
 * is safe and produces a VM_TYPE_ERROR diagnostic. */
#if __STDC_HOSTED__
#  include <assert.h>
#  define URBI_DISPATCH_ASSERT(cond) assert(cond)
#else
#  define URBI_DISPATCH_ASSERT(cond) ((void)0)
#endif

/* Generic unsupported-opcode error message.  Used by placeholder arms
 * that will be replaced by real implementations in later tasks. */
static void vm_format_type_error_msg(UVM *vm, const char *msg) {
    DiagWriter w;
    diag_init(&w, vm->last_errmsg, UVM_ERRMSG_CAP);
    diag_write_cstr(&w, "TypeError: ");
    diag_write_cstr(&w, msg);
}

/* --- dispatch_loop_until_yield ---
   The core execution engine (T6).  Runs s's bytecode until one of:
   - strand reaches DEAD (top-level OP_RET or halt_error)
   - strand yields via OP_YIELD (state → READY)
   - step_budget_in opcodes are consumed (state remains RUNNING)
   Returns the number of opcodes consumed.

   The uvm_run() function below is a thin adapter that creates a transient
   UStrand, loops calling this function until DEAD, then tears down.
   All dispatch state lives on the strand; vm holds only VM-wide state. */

uint64_t
dispatch_loop_until_yield(UStrand *s, uint64_t step_budget_in)
{
    UVM *vm = s->vm;
    uint64_t steps_consumed = 0;

    s->state = USTRAND_STATE_RUNNING;
    vm->step_budget_remaining = step_budget_in;

#if UVM_USE_COMPUTED_GOTO
    /* Dispatch table keyed by opcode.  All opcodes populated; loader
       validates opcode is in [0, OP_MAX) before uvm_run is called. */
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
        /* M3 row 7 control-transfer — T10 wires THROW/TRY_BEGIN/TRY_END/RESUME/LOAD_CATCH_VALUE
         * T11 wires PUSH_TAG/POP_TAG/PUSH_FRAME_GUARD; TAG_STOP stays stub until T31. */
        [OP_THROW]            = &&label_OP_THROW,
        [OP_TAG_STOP]         = &&label_row7_stub,
        [OP_TRY_BEGIN]        = &&label_OP_TRY_BEGIN,
        [OP_TRY_END]          = &&label_OP_TRY_END,
        [OP_PUSH_TAG]         = &&label_OP_PUSH_TAG,
        [OP_POP_TAG]          = &&label_OP_POP_TAG,
        [OP_PUSH_FRAME_GUARD] = &&label_OP_PUSH_FRAME_GUARD,
        [OP_RESUME]           = &&label_OP_RESUME,
        [OP_LOAD_CATCH_VALUE] = &&label_OP_LOAD_CATCH_VALUE,
    };

    DISPATCH();
#else
dispatch:
    DISPATCH() {
#endif

        CASE(OP_LOADK) {
            s->R[uinstr_a(*s->pc)] = s->cur_consts[uinstr_bx(*s->pc)];
            NEXT();
        }

        CASE(OP_MOVE) {
            s->R[uinstr_a(*s->pc)] = s->R[uinstr_b(*s->pc)];
            NEXT();
        }

        CASE(OP_ADD) {
            UValue *a = &s->R[uinstr_a(*s->pc)];
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *cc = &s->R[uinstr_c(*s->pc)];
            UVMError rc = arith_add(a, b, cc);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm_format_type_error_binary(vm, s->module,
                    (size_t)(s->pc - s->pc_base),
                    OP_ADD, b->kind, cc->kind);
                HALT();
            }
            NEXT();
        }

        CASE(OP_SUB) {
            UValue *a = &s->R[uinstr_a(*s->pc)];
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *cc = &s->R[uinstr_c(*s->pc)];
            UVMError rc = arith_sub(a, b, cc);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm_format_type_error_binary(vm, s->module,
                    (size_t)(s->pc - s->pc_base),
                    OP_SUB, b->kind, cc->kind);
                HALT();
            }
            NEXT();
        }

        CASE(OP_MUL) {
            UValue *a = &s->R[uinstr_a(*s->pc)];
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *cc = &s->R[uinstr_c(*s->pc)];
            UVMError rc = arith_mul(a, b, cc);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm_format_type_error_binary(vm, s->module,
                    (size_t)(s->pc - s->pc_base),
                    OP_MUL, b->kind, cc->kind);
                HALT();
            }
            NEXT();
        }

        CASE(OP_DIV) {
            UValue *a = &s->R[uinstr_a(*s->pc)];
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *cc = &s->R[uinstr_c(*s->pc)];
            UVMError rc = arith_div(a, b, cc);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm_format_type_error_binary(vm, s->module,
                    (size_t)(s->pc - s->pc_base),
                    OP_DIV, b->kind, cc->kind);
                HALT();
            }
            NEXT();
        }

        CASE(OP_NEG) {
            UValue *a = &s->R[uinstr_a(*s->pc)];
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            UVMError rc = arith_neg(a, b);
            if (rc != UVM_OK) {
                vm->last_error = rc;
                vm_format_type_error_unary(vm, s->module,
                    (size_t)(s->pc - s->pc_base),
                    OP_NEG, b->kind);
                HALT();
            }
            NEXT();
        }

        CASE(OP_RET) {
            UValue retval = s->R[uinstr_a(*s->pc)];

            if (s->frame_count == 0) {
                /* Top-frame return — strand becomes DEAD.
                 * Adapter (uvm_run) extracts result via out_slot. */
                if (s->out_slot != NULL) {
                    *s->out_slot = retval;
                }
                s->state = USTRAND_STATE_DEAD;
                steps_consumed++;
                goto exit_strand;
            }

            /* Non-top-frame: hand off to walker.
             * M2's inline pop+deliver is now urbi_unwind()'s job (T8 bridging
             * stub; T9 replaces with the real 5-kind walker). */
            s->unwind_value   = retval;
            s->pending_unwind = UEXEC_RETURN;
            steps_consumed++;
            goto safepoint;
        }

        CASE(OP_LOADNIL) {
            s->R[uinstr_a(*s->pc)].kind = (uint8_t)UVAL_NIL;
            NEXT();
        }

        CASE(OP_LOADBOOL) {
            s->R[uinstr_a(*s->pc)].kind  = (uint8_t)UVAL_BOOL;
            s->R[uinstr_a(*s->pc)].v.i   = uinstr_b(*s->pc) != 0 ? 1 : 0;
            if (uinstr_c(*s->pc)) { s->pc++; }
            NEXT();
        }

        CASE(OP_LOADVOID) {
            s->R[uinstr_a(*s->pc)].kind = (uint8_t)UVAL_VOID;
            NEXT();
        }

        CASE(OP_GETUPVAL) {
            /* ABC: R[A] := upvalue[B] from the current frame's closure. */
            UClosure *cur_cl = (s->frame_count > 0)
                             ? s->frames[s->frame_count - 1].closure
                             : NULL;
            if (cur_cl == NULL) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "GETUPVAL: no closure in current frame");
                HALT();
            }
            {
                uint8_t b = uinstr_b(*s->pc);
                UUpvalCell *uvc = cur_cl->upvals[b];
                s->R[uinstr_a(*s->pc)] = uvc->on_heap ? uvc->u.value
                                                      : *uvc->u.stack_ptr;
            }
            NEXT();
        }

        CASE(OP_SETUPVAL) {
            /* ABC: upvalue[B] := R[A] for the current frame's closure. */
            UClosure *cur_cl = (s->frame_count > 0)
                             ? s->frames[s->frame_count - 1].closure
                             : NULL;
            if (cur_cl == NULL) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "SETUPVAL: no closure in current frame");
                HALT();
            }
            {
                uint8_t a = uinstr_a(*s->pc);
                uint8_t b = uinstr_b(*s->pc);
                UUpvalCell *uvc = cur_cl->upvals[b];
                /* TODO(M4): wire urbi_gc_upvalue_write here once UClosure embeds UCell as first member. */
                if (uvc->on_heap) {
                    uvc->u.value = s->R[a];
                } else {
                    *uvc->u.stack_ptr = s->R[a];
                }
            }
            NEXT();
        }

        CASE(OP_CLOSURE) {
            /* ABx: R[A] := new closure from module->nested[Bx].
             * Reads nupvals pseudo-instructions (OP_MOVE-encoded) immediately
             * after, each specifying (B=in_stack, C=src_idx). */
            uint8_t  a  = uinstr_a(*s->pc);
            uint16_t bx = uinstr_bx(*s->pc);
            if ((size_t)bx >= s->module->nested_count) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "CLOSURE: proto index out of range");
                HALT();
            }
            UProto *child_proto = s->module->nested[bx];
            UClosure *cl = vm_alloc_closure(vm, child_proto, &s->closure_list);
            if (cl == NULL) {
                vm->last_error = UVM_OOM;
                vm_format_oom(vm, sizeof(UClosure));
                HALT();
            }
            /* Read nupvals pseudo-instructions. */
            {
                int i;
                for (i = 0; i < (int)child_proto->nupvals; i++) {
                    s->pc++;
                    uint8_t in_stack = uinstr_b(*s->pc);
                    uint8_t src_idx  = uinstr_c(*s->pc);
                    if (in_stack) {
                        UUpvalCell *uvc = vm_open_upvalue(vm, s, &s->R[src_idx]);
                        if (uvc == NULL) {
                            vm->alloc_fn(cl, 0, vm->alloc_ud);
                            vm->last_error = UVM_OOM;
                            vm_format_oom(vm, sizeof(UUpvalCell));
                            HALT();
                        }
                        cl->upvals[i] = uvc;
                    } else {
                        /* Re-capture: copy parent closure's upvalue pointer. */
                        UClosure *par_cl = (s->frame_count > 0)
                                         ? s->frames[s->frame_count - 1].closure
                                         : NULL;
                        if (par_cl == NULL || src_idx >= par_cl->nupvals) {
                            vm->alloc_fn(cl, 0, vm->alloc_ud);
                            vm->last_error = UVM_TYPE_ERROR;
                            vm_format_type_error_msg(vm, "CLOSURE: upvalue re-capture out of range");
                            HALT();
                        }
                        cl->upvals[i] = par_cl->upvals[src_idx];
                    }
                }
            }
            s->R[a].kind  = (uint8_t)UVAL_CLOSURE;
            s->R[a].v.p   = cl;
            NEXT();
        }

        CASE(OP_CLOSE) {
            /* ABC: heapify all open upvalue cells at R >= R[A]. */
            vm_close_upvalues(s, &s->R[uinstr_a(*s->pc)], &s->closed_cells);
            NEXT();
        }

        CASE(OP_CALL) {
            /* ABC: R[A](R[A+1]..R[A+B-1]); B-1 = nargs, C unused at T14.
             * After the call, the result overwrites R[A]. */
            uint8_t a = uinstr_a(*s->pc);
            uint8_t b = uinstr_b(*s->pc);
            int nargs = (int)b - 1;

            if (s->R[a].kind != (uint8_t)UVAL_CLOSURE) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "CALL: callee is not a closure");
                HALT();
            }
            UClosure *callee = (UClosure *)s->R[a].v.p;
            if (nargs != (int)callee->proto->nparams) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "CALL: wrong argument count");
                HALT();
            }
            if (s->frame_count >= UVM_MAX_FRAMES) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "CALL: call stack overflow");
                HALT();
            }

            /* Check stack space: callee's frame starts at R[a+1]. */
            if ((s->R + a + 1 + callee->proto->max_reg + 1) > (s->stack + UVM_STACK_CAP)) {
                vm->last_error = UVM_OOM;
                vm_format_oom(vm, (size_t)(callee->proto->max_reg + 1) * sizeof(UValue));
                HALT();
            }

            /* Push a new frame record.  This frame record stores what to restore
             * on OP_RET, plus the callee's closure (for GETUPVAL during the call). */
            UCallFrame *new_frame = &s->frames[s->frame_count++];
            new_frame->closure         = callee;
            new_frame->proto           = callee->proto;
            new_frame->pc              = s->pc;    /* points AT OP_CALL in caller */
            new_frame->base            = s->R;     /* caller's register base */
            new_frame->result_dest_reg = (int)a;  /* where to write result */

            /* Switch to callee frame. Args R[a+1..a+nargs] become R[0..nargs-1]. */
            s->R        = &s->R[a + 1];
            s->pc       = callee->proto->instructions;
            s->pc_base  = s->pc;
            s->cur_consts = callee->proto->constants ? callee->proto->constants
                                                     : s->module->constants;

            /* Zero registers beyond nparams up to max_reg. */
            {
                int si;
                for (si = nargs; si <= (int)callee->proto->max_reg; si++) {
                    UValue z = {0};
                    s->R[si] = z;
                }
            }

            /* Safepoint at call-frame-push. */
            steps_consumed++;
            goto safepoint;
        }

        CASE(OP_JMP) {
            /* ABx: pc += signed(Bx) - 32768.  Offset is applied after the
             * normal pc++ in NEXT, so we pre-adjust by (offset - 1). */
            int offset = (int)uinstr_bx(*s->pc) - 32768;
            s->pc += offset;
            /* Safepoint at backward branch (prevents infinite loop starvation). */
            if (offset < 0) {
                steps_consumed++;
                goto safepoint;
            }
            NEXT();
        }

        CASE(OP_TEST) {
            /* ABC: if (truthy(R[A]) == C) pc++ (skip next instr) */
            const UValue *a = &s->R[uinstr_a(*s->pc)];
            bool truthy = uvalue_truthy(a);
            if ((int)truthy == (int)uinstr_c(*s->pc)) { s->pc++; }
            NEXT();
        }

        CASE(OP_TESTSET) {
            /* ABC: if (truthy(R[B]) == C) pc++ else R[A] := R[B] */
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            bool truthy = uvalue_truthy(b);
            if ((int)truthy == (int)uinstr_c(*s->pc)) {
                s->pc++;
            } else {
                s->R[uinstr_a(*s->pc)] = *b;
            }
            NEXT();
        }

        CASE(OP_EQ) {
            /* ABC: if ((R[B]==R[C]) != A) pc++ */
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *c = &s->R[uinstr_c(*s->pc)];
            bool eq = uvalue_equal(b, c);
            if ((int)eq != (int)uinstr_a(*s->pc)) { s->pc++; }
            NEXT();
        }

        CASE(OP_NEQ) {
            /* ABC: if ((R[B]!=R[C]) != A) pc++ */
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *c = &s->R[uinstr_c(*s->pc)];
            bool neq = !uvalue_equal(b, c);
            if ((int)neq != (int)uinstr_a(*s->pc)) { s->pc++; }
            NEXT();
        }

        CASE(OP_LT) {
            /* ABC: if ((R[B]<R[C]) != A) pc++ — numeric only at M2 */
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *c = &s->R[uinstr_c(*s->pc)];
            bool lt = false;
            if (uvalue_lt(b, c, &lt) != UVAL_CMP_OK) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_binary(vm, s->module,
                    (size_t)(s->pc - s->pc_base), OP_LT, b->kind, c->kind);
                HALT();
            }
            if ((int)lt != (int)uinstr_a(*s->pc)) { s->pc++; }
            NEXT();
        }

        CASE(OP_LE) {
            /* ABC: if ((R[B]<=R[C]) != A) pc++ — numeric only at M2 */
            const UValue *b = &s->R[uinstr_b(*s->pc)];
            const UValue *c = &s->R[uinstr_c(*s->pc)];
            bool le = false;
            if (uvalue_le(b, c, &le) != UVAL_CMP_OK) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_binary(vm, s->module,
                    (size_t)(s->pc - s->pc_base), OP_LE, b->kind, c->kind);
                HALT();
            }
            if ((int)le != (int)uinstr_a(*s->pc)) { s->pc++; }
            NEXT();
        }

        CASE(OP_YIELD) {
            /* Cooperative yield: advance past this opcode, transition to READY,
               and return to the scheduler.  The uvm_run adapter re-enters
               dispatch_loop_until_yield until strand is DEAD. */
            s->pc++;
            s->state = USTRAND_STATE_READY;
            sched_strand_yield(s);
            steps_consumed++;
            goto exit_strand;
        }

        CASE(OP_FORK_DETACH) {
            /* M3 `,` separator runtime. Structural placeholder. */
            vm->last_error = UVM_TYPE_ERROR;
            vm_format_type_error_msg(vm, "FORK_DETACH: not implemented until M3");
            HALT();
        }

        CASE(OP_FORK_JOIN) {
            /* M3 `&` separator runtime. Structural placeholder. */
            vm->last_error = UVM_TYPE_ERROR;
            vm_format_type_error_msg(vm, "FORK_JOIN: not implemented until M3");
            HALT();
        }

        CASE(OP_JOIN_WAIT) {
            /* M3 `&` join-point. Structural placeholder. */
            vm->last_error = UVM_TYPE_ERROR;
            vm_format_type_error_msg(vm, "JOIN_WAIT: not implemented until M3");
            HALT();
        }

        CASE(OP_GETSLOT) {
            /* M4 slot read. Structural placeholder. */
            vm->last_error = UVM_TYPE_ERROR;
            vm_format_type_error_msg(vm, "GETSLOT: not implemented until M4");
            HALT();
        }

        CASE(OP_SETSLOT) {
            /* M4 slot write. Structural placeholder. */
            vm->last_error = UVM_TYPE_ERROR;
            vm_format_type_error_msg(vm, "SETSLOT: not implemented until M4");
            HALT();
        }

        /* --- M3 row 7 control-transfer opcodes (T10 real dispatch) --- */

        CASE(OP_THROW) {
            /* OP_THROW ABx: A = reg_value, Bx = 0 (unused).
             * Set pending_unwind = UEXEC_THROW and unwind_value = R[A],
             * then go to safepoint where urbi_unwind() will walk the
             * cleanup stack. */
            uint8_t a = uinstr_a(*s->pc);
            s->unwind_value   = s->R[a];
            s->pending_unwind = UEXEC_THROW;
            s->pc++;
            goto safepoint;
        }

        CASE(OP_TRY_BEGIN) {
            /* OP_TRY_BEGIN ABx: A = flags, Bx = handler_pc.
             * Push a UCLEANUP_TRY_FRAME entry onto the cleanup stack. */
            uint8_t  flags      = uinstr_a(*s->pc);
            uint16_t handler_pc = uinstr_bx(*s->pc);
            UCleanupEntry *entry = strand_cleanup_push(s);
            if (entry == NULL) {
                /* Cleanup stack full — strand fatal. */
                s->fatal_status = UEXEC_THROW;
                s->state        = USTRAND_STATE_DEAD;
                goto exit_strand;
            }
            entry->kind           = (uint8_t)UCLEANUP_TRY_FRAME;
            entry->flags          = flags;
            entry->handler_pc     = handler_pc;
            entry->register_base  = 0u;
            entry->register_count = 0u;
            entry->owning_tag     = NULL;
            entry->catch_pattern  = NULL;
            entry->next_member    = NULL;
            entry->strand_back    = NULL;
            NEXT();
        }

        CASE(OP_TRY_END) {
            /* OP_TRY_END ABC: no operands.  Pop the top UCLEANUP_TRY_FRAME entry. */
            if (s->cleanup_depth > 0) {
                strand_cleanup_pop(s, UCLEANUP_TRY_FRAME);
            }
            NEXT();
        }

        CASE(OP_RESUME) {
            /* OP_RESUME: end of a finally/cleanup body.
             * Exits dispatch_loop_until_yield so run_cleanup_with_replace()
             * can check pending_unwind and restore the saved unwind state.
             * State stays RUNNING; caller (run_cleanup_with_replace) handles
             * the transition. */
            s->pc++;
            goto exit_strand;
        }

        CASE(OP_LOAD_CATCH_VALUE) {
            /* OP_LOAD_CATCH_VALUE ABC: A = destination register, B = C = 0.
             * Load s->catch_value (written by urbi_unwind on catch absorption)
             * into R[A].  First instruction of every catch handler body. */
            s->R[uinstr_a(*s->pc)] = s->catch_value;
            NEXT();
        }

        CASE(OP_PUSH_TAG) {
            /* OP_PUSH_TAG ABx:
             *   A[7:4] = flags nibble (0 at M3 — no FLAG_HAS_ONLEAVE)
             *   A[3:0] = tag_reg nibble (register holding the tag value)
             *   Bx     = onleave_pc (handler PC; 0 at M3 since no onleave body)
             *
             * Push a UCLEANUP_TAG_SCOPE entry onto the cleanup stack.
             * owning_tag = NULL at M3 (T29 wires real UTag pointer).
             * strand_back = s for future tag.stop() walk (T31 uses). */
            uint8_t  a          = uinstr_a(*s->pc);
            uint8_t  flags      = (uint8_t)((a >> 4) & 0xFu);
            uint16_t handler_pc = uinstr_bx(*s->pc);
            UCleanupEntry *entry = strand_cleanup_push(s);
            if (entry == NULL) {
                s->fatal_status = UEXEC_THROW;
                s->state        = USTRAND_STATE_DEAD;
                goto exit_strand;
            }
            entry->kind           = (uint8_t)UCLEANUP_TAG_SCOPE;
            entry->flags          = flags;
            entry->handler_pc     = handler_pc;
            entry->register_base  = 0u;
            entry->register_count = 0u;
            entry->owning_tag     = NULL;  /* T29 wires real UTag */
            entry->catch_pattern  = NULL;
            entry->next_member    = NULL;
            entry->strand_back    = s;
            NEXT();
        }

        CASE(OP_POP_TAG) {
            /* OP_POP_TAG ABC: A = tag_reg (unused at M3), B = C = 0.
             * Pop the top UCLEANUP_TAG_SCOPE entry.
             * If FLAG_HAS_ONLEAVE is set in the entry's flags, the onleave
             * handler would run via run_cleanup_with_replace — but at M3
             * flags is always 0 (no onleave body is emitted), so the handler
             * branch is dead code.  Include the check for forward-compatibility. */
            if (s->cleanup_depth > 0) {
                UCleanupEntry *top = &s->cleanup_base[s->cleanup_depth - 1];
                if ((top->flags & FLAG_HAS_ONLEAVE) != 0u) {
                    /* onleave handler: deferred to T30 — not reachable at M3.
                     * If somehow reached (bytecode corruption), halt safely. */
                    vm->last_error = UVM_TYPE_ERROR;
                    vm_format_type_error_msg(vm, "POP_TAG: FLAG_HAS_ONLEAVE not wired until T30");
                    HALT();
                }
                strand_cleanup_pop(s, UCLEANUP_TAG_SCOPE);
            }
            NEXT();
        }

        CASE(OP_PUSH_FRAME_GUARD) {
            /* OP_PUSH_FRAME_GUARD ABC: A = register_base, B = register_count, C = 0.
             * Push a UCLEANUP_CALL_FRAME entry onto the cleanup stack.
             * The T9 unwind walker absorbs UEXEC_RETURN at CALL_FRAME entries,
             * delivering the return value and popping the frame.
             * strand_back = s for compatibility with unwind walker. */
            uint8_t register_base  = uinstr_a(*s->pc);
            uint8_t register_count = uinstr_b(*s->pc);
            UCleanupEntry *entry = strand_cleanup_push(s);
            if (entry == NULL) {
                s->fatal_status = UEXEC_THROW;
                s->state        = USTRAND_STATE_DEAD;
                goto exit_strand;
            }
            entry->kind           = (uint8_t)UCLEANUP_CALL_FRAME;
            entry->flags          = 0u;
            entry->handler_pc     = 0u;
            entry->register_base  = register_base;
            entry->register_count = register_count;
            entry->owning_tag     = NULL;
            entry->catch_pattern  = NULL;
            entry->next_member    = NULL;
            entry->strand_back    = s;
            NEXT();
        }

        /* OP_TAG_STOP stays as a stub — T31 (urbi_tag_stop) wires the runtime.
         * No syntax emits this opcode yet at T11. */
#if UVM_USE_COMPUTED_GOTO
        label_row7_stub:
#else
        case OP_TAG_STOP:
#endif
        {
            URBI_DISPATCH_ASSERT(0 && "OP_TAG_STOP runtime owned by T31");
            vm->last_error = UVM_TYPE_ERROR;
            vm_format_type_error_msg(vm, "OP_TAG_STOP: not yet implemented (T31)");
            HALT();
        }

#if !UVM_USE_COMPUTED_GOTO
        default: {
            /* Unreachable — loader rejects unknown opcodes before uvm_run
               is called. The default: branch satisfies -Wswitch-enum. */
            vm->last_error = UVM_TYPE_ERROR;
            HALT();
        }
    }
#endif

    /* Unreachable for computed-goto path; switch path falls through from
       every NEXT() which ends with goto dispatch above. */

halt_error:
    /* Error path: strand is now dead. */
    s->state = USTRAND_STATE_DEAD;
    steps_consumed++;
    goto exit_strand;

safepoint:
    /* Safepoint actions (run at backward-branch, call, and non-top OP_RET).
       Order: unwind check → per-strand budget → VM-wide budget → GC → hooks. */
    if (s->pending_unwind != UEXEC_OK) {
        urbi_unwind(s);
        if (s->state == USTRAND_STATE_DEAD) goto exit_strand;
    }
    if (s->instruction_budget_remaining == 0) {
        s->state = USTRAND_STATE_READY;
        sched_strand_yield(s);
        goto exit_strand;
    }
    s->instruction_budget_remaining--;
    if (vm->step_budget_remaining == 0) {
        /* Budget exhausted from caller's perspective; state stays RUNNING.
           The uvm_run adapter treats RUNNING-but-exit as "continue". */
        goto exit_strand;
    }
    vm->step_budget_remaining--;
    if (vm->gc_pending)           gc_slice(vm, URBI_GC_SLICE_BUDGET);
    if (vm->pending_onleave_head) drain_pending_onleave_queue(vm);
    if (vm->watcher_dirty_count > 0) watcher_eval_dirty(vm);
    /* Preemption flag reserved for v2; not checked at M3. */
    /* Resume dispatch. */
#if UVM_USE_COMPUTED_GOTO
    DISPATCH();
#else
    goto dispatch;
#endif

exit_strand:
    /* strand_runnable_count ownership at exit:
     *   - uvm_run transient strands are not tracked in strand_runnable_count
     *     (they bypass sched_strand_make_runnable). The READY-cycle increment
     *     via sched_strand_yield is balanced by the dequeue decrement in the
     *     uvm_run loop (src/uvm.c, the strand_runnable_count-- block).
     *   - T16 urbi_step driver: strands dequeued from the ready queue before
     *     entering dispatch_loop_until_yield. T16 decrements strand_runnable_count
     *     in the driver after dispatch returns with state == USTRAND_STATE_DEAD,
     *     keeping the decrement co-located with the dequeue logic.
     *   - sched_strand_block handles RUNNING → WAITING decrements inline.
     * No decrement here; see T16 for the scheduler-driven DEAD-path decrement. */
    return steps_consumed;
}

/* --- uvm_run: thin adapter that wraps dispatch_loop_until_yield.
   Preserves the M2 public API contract:
   - Resets error state at entry.
   - Frees the previous run's return closure.
   - Returns UVM_OK with *out set on success, or the error code on failure.
   - Keeps vm->last_return_closure alive for the caller to inspect.

   Implementation note: uvm_run kept in uvm.c (not split to udispatch.c)
   at T6 per plan recommendation; T20 may revisit when strand C API matures. */

UVMError uvm_run(UVM *vm, const UModule *module, UValue *out) {
    /* Reset error state at entry so callers who run multiple modules
       don't see stale last_error from a prior failure. */
    vm->last_error = UVM_OK;
    vm->last_errmsg[0] = '\0';

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

    /* Create a transient strand for this run.
       We zero-init manually rather than calling ustrand_init to avoid
       pre-allocating the cleanup stack (which unwind_walk wires at T9;
       the M2 baseline dispatcher never uses it).  This preserves the
       M2 contract that the first allocation failure returns OOM for the
       register stack, not the cleanup stack. */
    UStrand strand;
    {
        volatile unsigned char *p = (volatile unsigned char *)&strand;
        size_t n = sizeof(strand);
        size_t i;
        for (i = 0; i < n; i++) p[i] = 0;
    }
    strand.vm    = vm;
    strand.state = USTRAND_STATE_DORMANT;

    /* Allocate the per-strand register stack first (preserves M2 OOM contract:
     * first allocation failure → UVM_OOM with diagnostic before cleanup init). */
    const size_t stack_bytes = UVM_STACK_CAP * sizeof(UValue);
    strand.stack = (UValue *)vm->alloc_fn(NULL, stack_bytes, vm->alloc_ud);
    if (strand.stack == NULL) {
        vm->last_error = UVM_OOM;
        vm_format_oom(vm, stack_bytes);
        ustrand_destroy(&strand, vm);
        return UVM_OOM;
    }
    vm_zero(strand.stack, stack_bytes);

    /* T10: initialise the cleanup stack so OP_TRY_BEGIN can push entries.
     * Failure leaves cleanup_base=NULL; OP_TRY_BEGIN detects and halts safely. */
    (void)strand_cleanup_stack_init(&strand, vm, URBI_CLEANUP_MAX);

    /* Wire frame-0 from module. */
    strand.R          = strand.stack;
    strand.pc         = module->instructions;
    strand.pc_base    = module->instructions;
    strand.cur_consts = module->constants;
    strand.module     = module;
    strand.frame_count = 0;
    strand.open_upvals = NULL;
    strand.closure_list = NULL;
    strand.closed_cells = NULL;
    strand.out_slot   = out;  /* OP_RET at top-frame writes *out_slot */
    strand.state      = USTRAND_STATE_RUNNING;

    /* Run to completion: loop until strand is DEAD or a fatal error sets last_error.
       OP_YIELD or per-strand budget exhaustion leaves state READY — treat as
       "continue" for the M2 API contract (uvm_run must block until completion). */
    for (;;) {
        (void)dispatch_loop_until_yield(&strand, /* step_budget */ UINT64_MAX);
        if (strand.state == USTRAND_STATE_DEAD) break;
        if (vm->last_error != UVM_OK) break;
        if (strand.state == USTRAND_STATE_READY) {
            /* OP_YIELD (between separator children) or per-strand budget.
               Remove from ready queue (sched_strand_yield enqueued it),
               reset to RUNNING, and re-enter dispatch. */
            if (vm->ready_head == &strand) {
                vm->ready_head = strand.ready_next;
                if (vm->ready_head != NULL)
                    vm->ready_head->ready_prev = NULL;
                else
                    vm->ready_tail = NULL;
                if (vm->strand_runnable_count > 0)
                    vm->strand_runnable_count--;
                strand.ready_next = NULL;
                strand.ready_prev = NULL;
            }
            strand.state = USTRAND_STATE_RUNNING;
            continue;
        }
        if (USTRAND_IS_WAITING(&strand)) {
            /* M2 baseline has no blocking opcodes; WAITING here is a bug. */
            vm->last_error = UVM_TYPE_ERROR;
            vm_format_type_error_msg(vm, "strand blocked unexpectedly in uvm_run");
            break;
        }
        /* RUNNING with step_budget exhausted (UINT64_MAX → shouldn't happen). */
        break;
    }

    /* Pre-GC: free every closure allocated this run, except the one returned
     * to the caller via *out.  That closure is kept alive in
     * vm->last_return_closure until the next uvm_run() or uvm_destroy(). */
    {
        UClosure *out_cl = (out->kind == (uint8_t)UVAL_CLOSURE)
                           ? (UClosure *)out->v.p : NULL;
        vm->last_return_closure = out_cl;

        UClosure *cl = strand.closure_list;
        while (cl != NULL) {
            UClosure *next = cl->next_alloc;
            if (cl != out_cl) {
                vm->alloc_fn(cl, 0, vm->alloc_ud);
            }
            cl = next;
        }
    }

    /* Pre-GC: free every heapified upvalue cell allocated this run. */
    {
        UUpvalCell *cell = strand.closed_cells;
        while (cell != NULL) {
            UUpvalCell *next = cell->next;
            vm->alloc_fn(cell, 0, vm->alloc_ud);
            cell = next;
        }
    }

    /* Free any open upvalue cells still on the strand. */
    vm_free_open_upvalues(vm, &strand);

    /* Free the register stack. */
    vm->alloc_fn(strand.stack, 0, vm->alloc_ud);
    strand.stack = NULL;

    UVMError rc = vm->last_error;
    ustrand_destroy(&strand, vm);
    return rc;
}
