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
#include "urbi/urbi.h"    /* URBI_CALLBACK_WARN_US, URBI_WATCHDOG_WARN */
#include "uclosure.h"     /* UClosure full definition (M4: embeds UCell) */
#include "ustrand.h"
#include "uintern.h"
#include "uvalue.h"
#include "sched/usched_cooperative.h"
#include "uunwind.h"
#include "realm/urealm.h"
#include "uevent_ring.h"
#include "urbi/gc.h" /* urbi_gc_slice + URBI_GC_SLICE_BUDGET */
#include "uhandle.h" /* host_handle_walk_roots (T27) */
#include "utag.h"    /* UTag, utag_create/destroy (T30) */
#include "watcher/uwatcher.h" /* uwatcher_pool_init/destroy (T32) */
#include "uop_fork.h" /* op_fork_detach/join/wait + fork_wake_joiners (T38) */
#include "object/utypes_init.h" /* urbi_object_builtin_types_init (M4) */
#include "object/uic.h"         /* UIC + urbi_slot_get_slow / urbi_slot_set_slow (T22-T25) */
#include "object/uobject.h"     /* UObject — receivers for GETSLOT/SETSLOT (T22-T25) */
#include "object/umoduleinstance.h" /* urbi_get_or_create_module_instance (M4 follow-up) */

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
    vm->topology_gen   = 1ull;   /* pre-M4 topology spec §3.1: init=1, 0 reserved */
    vm->lookup_id      = 1ull;   /* pre-M4 prototype-chain spec §7.1 */
    vm->next_object_id = 0u;     /* pre-M4 prototype-chain spec §8.1 (first alloc → 1) */
    vm->root_shape     = NULL;   /* lazy-allocated by urbi_shape_root */

    /* M4 atom-family singletons (T8): all NULL until first lazy-create. */
    vm->atom_object  = NULL;
    vm->atom_integer = NULL;
    vm->atom_float   = NULL;
    vm->atom_string  = NULL;
    vm->atom_list    = NULL;
    vm->atom_dict    = NULL;
    vm->atom_tag     = NULL;
    vm->atom_event   = NULL;
    vm->atom_symbol  = NULL;

    /* M4 T30 — UModuleInstance registry head: empty until first
     * urbi_module_instance_create. */
    vm->module_instances_head = NULL;

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
    vm->realm_id_seq = 0u;

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

    /* Register default root providers.
     * Order: scheduler, realm, intern, host-handle, watcher table. */
    urbi_gc_register_root_provider(vm, sched_walk_roots);
    urbi_gc_register_root_provider(vm, realm_list_walk_roots);
    urbi_gc_register_root_provider(vm, intern_table_walk_roots);
    urbi_gc_register_root_provider(vm, host_handle_walk_roots);
    urbi_gc_register_root_provider(vm, watcher_table_walk_roots);

    /* Type table + host-handle table. */
    {
        int i;
        for (i = 0; i < 256; i++) {
            vm->type_table[i] = NULL;
        }
    }
    vm->host_type_count      = 0u;

    /* Register built-in M4 object-model UType descriptors directly into
     * vm->type_table[].  Built-in tags can't go through urbi_register_type
     * (which guards tags < UTYPE_HOST_BASE per src/utype.c). */
    urbi_object_builtin_types_init(vm);

    /* T36: register the M4 GC root provider for atom singletons +
     * vm->root_shape + the UModuleInstance chain.  Replaces the manual
     * urbi_pin calls on atom singletons that lived in T8.  Must come
     * after the type-table setup so the walker's gc_shade_gray invocations
     * find a registered UType for each cell. */
    urbi_object_register_gc_roots(vm);

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
    vm->test_watcher_condition_hook = NULL;
    vm->test_watcher_fire_hook      = NULL;
    vm->test_watcher_onleave_hook   = NULL;
    vm->pending_onleave_head   = NULL;
    vm->pending_onleave_tail   = NULL;

    /* Watcher pool: allocate after field zero-init and after GC init
     * so pool_alloc can use vm->current_white and vm->alloc_fn is set. */
    uwatcher_pool_init(vm);
    /* OOM note: if uwatcher_pool_init returns -1, watcher_pool_base stays NULL.
     * pool_alloc will return NULL on any install attempt — still safe. Matches the
     * event_ring OOM pattern from row 9 scheduler work: silently leaves the pointer
     * NULL; the install API returns NULL on first use, surfacing the failure at the
     * use site rather than at vm_init. The embedded caller should check
     * vm->watcher_pool_base != NULL post-init. */

    /* Scratch frame: one per VM, used by watcher_eval_dirty and (T35)
     * drain_pending_onleave_queue.  Allocated here so M5's real
     * urbi_run_closure_on_scratch can use it without a layout change.
     * Freed by uvm_destroy (already wired at T32; see comment there).
     * OOM: leave NULL and continue — matches event_ring + pool pattern.
     * Note: vm->alloc_fn is always non-NULL here due to lines 62-66 substitution. */
    {
        void *sf = vm->alloc_fn(NULL, sizeof(UScratchFrame), vm->alloc_ud);
        if (sf != NULL) {
            /* Zero-fill via volatile byte loop (freestanding: no memset). */
            volatile unsigned char *p = (volatile unsigned char *)sf;
            size_t i;
            for (i = 0; i < sizeof(UScratchFrame); i++) p[i] = 0;
        }
        vm->watcher_scratch_frame = sf;
    }

    /* Host time hook: default stub; embedded callers override post-init. */
    vm->host_time_us = default_host_time_us_stub;
}

void uvm_destroy(UVM *vm) {
    if (vm == NULL) return;

    /* --- M3 teardown stubs (in reverse-init order) ---
     * Subsystem-owned teardowns are deferred to their landing tasks. */
    urealm_teardown_all(vm);  /* T14: destroy all live Realms */
    uwatcher_pool_destroy(vm);  /* T32: free pool slab before GC */
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
typedef struct UDiagWriter {
    char   *buf;
    size_t  cap;   /* buffer capacity */
    size_t  used;  /* bytes written so far (excluding trailing NUL) */
    bool    truncated;
} UDiagWriter;

static void diag_init(UDiagWriter *w, char *buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->used = 0;
    w->truncated = false;
    if (cap > 0) buf[0] = '\0';
}

static void diag_write_cstr(UDiagWriter *w, const char *s) {
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
static void diag_write_u32(UDiagWriter *w, uint32_t n) {
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

static void diag_write_size(UDiagWriter *w, size_t n) {
    /* size_t is at most 64 bits on our targets; fits in u32 for any
       realistic frame size or pc. Cap for safety. */
    if (n > UINT32_MAX) n = UINT32_MAX;
    diag_write_u32(w, (uint32_t)n);
}

static void diag_write_kind_name(UDiagWriter *w, uint8_t kind) {
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
static void diag_write_prefix(UDiagWriter *w, const UModule *module, size_t pc) {
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
    UDiagWriter w;
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
    UDiagWriter w;
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
    UDiagWriter w;
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
 *
 * M4: UClosure embeds UCell at offset 0.  The cell header is initialised
 * here (type_tag = UTYPE_CLOSURE, gc_byte = vm->current_white) so that
 * urbi_gc_upvalue_write may safely cast UClosure* → UCell* and read a
 * valid color for the barrier check.  The closure is NOT enrolled on
 * vm->all_cells_head — lifetime stays with the strand's closure_list
 * (legacy free-list).  GC-managed allocation via urbi_gc_alloc is tracked
 * as a follow-up M4 task; it requires enrolling the transient uvm_run
 * strand as a GC root before closures stored in registers can survive
 * a mid-dispatch collection cycle.
 *
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
    /* Cell header (M4): well-formed for barrier safety even though the
     * closure is not on vm->all_cells_head at this commit. */
    cl->cell.type_tag = UTYPE_CLOSURE;
    cl->cell.gc_byte  = vm->current_white;
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
 * Declared non-static (exported via uvm.h) for uunwind.c access. */
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
    UDiagWriter w;
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
            /* ABC: R[A] := upvalue[B] from the current frame's closure.
             * At frame_count == 0 (top-level strand including fork-spawned
             * children) fall back to s->entry_closure so that closures
             * created by emit_lazy_thunk can read their captured upvalues. */
            UClosure *cur_cl = (s->frame_count > 0)
                             ? s->frames[s->frame_count - 1].closure
                             : s->entry_closure;
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
            /* ABC: upvalue[B] := R[A] for the current frame's closure.
             * At frame_count == 0 fall back to s->entry_closure (same
             * rationale as OP_GETUPVAL). */
            UClosure *cur_cl = (s->frame_count > 0)
                             ? s->frames[s->frame_count - 1].closure
                             : s->entry_closure;
            if (cur_cl == NULL) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "SETUPVAL: no closure in current frame");
                HALT();
            }
            {
                uint8_t a = uinstr_a(*s->pc);
                uint8_t b = uinstr_b(*s->pc);
                UUpvalCell *uvc = cur_cl->upvals[b];
                /* GC barrier (M4): UClosure now embeds UCell at offset 0,
                 * so urbi_gc_upvalue_write may safely cast UClosure* → UCell*
                 * for the color check.  Hook fires before the actual store. */
                urbi_gc_upvalue_write(vm, cur_cl, b, s->R[a]);
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
            /* M4 follow-up: bind proto_inst so the new closure can dispatch
             * OP_GETSLOT/OP_SETSLOT against the per-VM IC table.  entries[0]
             * is the root chunk; entries[bx + 1] is the matching nested proto. */
            if (s->module_instance != NULL
                && s->module_instance->proto_instances != NULL
                && (size_t)bx + 1u < (size_t)s->module_instance->proto_instances->n) {
                cl->proto_inst = &s->module_instance->proto_instances->entries[bx + 1u];
            }
            /* If no module_instance is bound (defensive — uvm_run wires it for
             * every normal execution path), proto_inst stays NULL and
             * OP_GETSLOT/SETSLOT will diagnose cleanly. */

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
                        /* Re-capture: copy parent closure's upvalue pointer.
                         * Fall back to entry_closure at frame_count == 0 for
                         * fork-spawned child strands (same as GETUPVAL). */
                        UClosure *par_cl = (s->frame_count > 0)
                                         ? s->frames[s->frame_count - 1].closure
                                         : s->entry_closure;
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
            /* `,` separator: spawn child closure as detached strand.
             * A = closure_reg.  Parent continues; child runs concurrently.
             * See src/uop_fork.c for M3 closure-spawn vs. spec §7.1 rationale.
             * Rejected from uvm_run's stack-local transient because that
             * adapter only dispatches its own strand and would leak any
             * spawned children.  T33 routes the transient onto
             * vm->global_realm->strands_head for GC-walker visibility, so
             * realm == NULL no longer discriminates; the dedicated flag
             * is_uvm_run_transient does. */
            if (s->is_uvm_run_transient) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "OP_FORK_DETACH: `,` requires urbi_step driver (uvm_run transient strand)");
                HALT();
            }
            int rc = op_fork_detach(s, vm, *s->pc);
            if (rc != 0) goto exit_strand;
            NEXT();
        }

        CASE(OP_FORK_JOIN) {
            /* `&` separator LHS: spawn child closure, store handle in R[B].
             * A = closure_reg, B = child_handle_reg.
             * Same uvm_run-transient guard as OP_FORK_DETACH; see note above. */
            if (s->is_uvm_run_transient) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "OP_FORK_JOIN: `&` requires urbi_step driver (uvm_run transient strand)");
                HALT();
            }
            int rc = op_fork_join(s, vm, *s->pc);
            if (rc != 0) goto exit_strand;
            NEXT();
        }

        CASE(OP_JOIN_WAIT) {
            /* `&` separator join-point: block until child handle in R[A] is DEAD.
             * A = child_handle_reg. */
            int rc = op_join_wait(s, vm, *s->pc);
            if (rc < 0) goto exit_strand;   /* OOM or error */
            if (rc > 0) {
                /* Blocked — parent threaded onto child->joiners_head. */
                steps_consumed++;
                goto exit_strand;
            }
            /* rc == 0: child already DEAD, continue. */
            NEXT();
        }

        CASE(OP_GETSLOT) {
            /* OP_GETSLOT ABC: R[A] := R[B].slot[ic_index].
             *   A = dst_reg, B = recv_reg, C = ic_index.
             *
             * Fast path: linear scan of the IC entries for a (recv->shape,
             * vm->topology_gen) match.  On hit, copy *slots[k] into R[A]
             * (or, if FLAG_OGET set, dispatch the getter — currently raises
             * a diagnostic; full getter dispatch lands when the frame-push
             * wrapper API matures, see TODO below).
             *
             * Slow path: urbi_slot_get_slow walks the prototype chain,
             * fills exactly one IC entry at ic->replace_cursor, and either
             * copies the slot value to *out (no OGET) or signals OGET-flag
             * present (caller would dispatch). */
            uint32_t i = *s->pc;
            uint8_t  dst_reg  = uinstr_a(i);
            uint8_t  recv_reg = uinstr_b(i);
            uint8_t  ic_index = uinstr_c(i);

            /* Resolve IC table:
             *   frame_count == 0 (top-level / root chunk):
             *       use s->module_instance->proto_instances->entries[0]
             *   frame_count > 0 (nested call):
             *       use frames[top].closure->proto_inst (set by OP_CLOSURE) */
            UProtoInstance *pi = NULL;
            if (s->frame_count == 0) {
                if (s->module_instance != NULL
                    && s->module_instance->proto_instances != NULL) {
                    pi = &s->module_instance->proto_instances->entries[0];
                }
            } else {
                UClosure *cur_cl = s->frames[s->frame_count - 1].closure;
                if (cur_cl != NULL) pi = cur_cl->proto_inst;
            }
            if (pi == NULL || pi->ic_table == NULL) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "GETSLOT: no IC table bound");
                HALT();
            }
            UIC *ic = &pi->ic_table[ic_index];

            if (s->R[recv_reg].kind != (uint8_t)UVAL_OBJECT) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "GETSLOT: receiver is not an Object");
                HALT();
            }
            UObject *recv = (UObject *)s->R[recv_reg].v.p;

            /* Fast path: linear scan over ic->n entries. */
            for (uint8_t k = 0; k < ic->n; k++) {
                if (ic->recv_shapes[k]  == recv->shape
                 && ic->topology_gen[k] == vm->topology_gen) {
                    if (ic->flags[k] & URBI_SLOT_FLAG_OGET) {
                        /* TODO(T39+): wire URBI_VM_DISPATCH_GETTER once the
                         * frame-push wrapper for receiver+0-arg invocation
                         * is defined.  Until then, getters are rejected at
                         * dispatch with a clean diagnostic; corpus revival
                         * fixtures (T42) exercise non-getter slot reads.
                         * The IC entry itself was filled correctly by the
                         * slow path on first install, so the diagnostic is
                         * a runtime-only restriction, not a missing IC. */
                        vm->last_error = UVM_TYPE_ERROR;
                        vm_format_type_error_msg(vm, "GETSLOT: getter dispatch not yet implemented");
                        HALT();
                    }
                    s->R[dst_reg] = *ic->slots[k];
                    NEXT();
                }
            }

            /* Slow path. */
            UValue v;
            int rc = urbi_slot_get_slow(vm, recv, ic, &v);
            if (rc != 0) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "GETSLOT: slot lookup failed");
                HALT();
            }
            /* Inspect the just-filled IC entry to decide if a getter is
             * pending.  Same TODO as above — diagnose for now. */
            uint8_t fresh_k = (uint8_t)((ic->replace_cursor + URBI_IC_ENTRIES_PER_SITE - 1u)
                                        % URBI_IC_ENTRIES_PER_SITE);
            if (ic->n > 0u && (ic->flags[fresh_k] & URBI_SLOT_FLAG_OGET)) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "GETSLOT: getter dispatch not yet implemented");
                HALT();
            }
            s->R[dst_reg] = v;
            NEXT();
        }

        CASE(OP_SETSLOT) {
            /* OP_SETSLOT ABC: R[B].slot[ic_index] := R[A].
             *   A = src_reg, B = recv_reg, C = ic_index.
             *
             * Fast path: scan IC entries; on shape+topology match, dispatch
             * setter (FLAG_OSET — currently diagnoses), reject CONSTANT, or
             * write in place if FLAG_LOCAL.  A proto-chain hit (no LOCAL,
             * no OSET) breaks out of the fast path so the slow path can do
             * COW.
             *
             * Slow path: urbi_slot_set_slow walks the prototype chain and
             * either installs a fresh local slot on recv (miss / COW) or
             * fills the IC and writes through (local hit / setter pending). */
            uint32_t i = *s->pc;
            uint8_t  src_reg  = uinstr_a(i);
            uint8_t  recv_reg = uinstr_b(i);
            uint8_t  ic_index = uinstr_c(i);

            /* Resolve IC table:
             *   frame_count == 0 (top-level / root chunk):
             *       use s->module_instance->proto_instances->entries[0]
             *   frame_count > 0 (nested call):
             *       use frames[top].closure->proto_inst (set by OP_CLOSURE) */
            UProtoInstance *pi = NULL;
            if (s->frame_count == 0) {
                if (s->module_instance != NULL
                    && s->module_instance->proto_instances != NULL) {
                    pi = &s->module_instance->proto_instances->entries[0];
                }
            } else {
                UClosure *cur_cl = s->frames[s->frame_count - 1].closure;
                if (cur_cl != NULL) pi = cur_cl->proto_inst;
            }
            if (pi == NULL || pi->ic_table == NULL) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "SETSLOT: no IC table bound");
                HALT();
            }
            UIC *ic = &pi->ic_table[ic_index];

            if (s->R[recv_reg].kind != (uint8_t)UVAL_OBJECT) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "SETSLOT: receiver is not an Object");
                HALT();
            }
            UObject *recv = (UObject *)s->R[recv_reg].v.p;
            UValue v = s->R[src_reg];

            int slow_path = 1;
            for (uint8_t k = 0; k < ic->n; k++) {
                if (ic->recv_shapes[k]  == recv->shape
                 && ic->topology_gen[k] == vm->topology_gen) {
                    if (ic->flags[k] & URBI_SLOT_FLAG_OSET) {
                        /* TODO(T39+): wire URBI_VM_DISPATCH_SETTER. */
                        vm->last_error = UVM_TYPE_ERROR;
                        vm_format_type_error_msg(vm, "SETSLOT: setter dispatch not yet implemented");
                        HALT();
                    }
                    if (ic->flags[k] & URBI_SLOT_FLAG_CONSTANT) {
                        vm->last_error = UVM_TYPE_ERROR;
                        vm_format_type_error_msg(vm, "SETSLOT: cannot write to constant slot");
                        HALT();
                    }
                    if (ic->flags[k] & URBI_SLOT_FLAG_LOCAL) {
                        /* Direct in-place write.  Forward Dijkstra barrier
                         * fires on the parent UObject (the cell containing
                         * ic->slots[k]).  Cast UCell* via the pinned
                         * UObject layout: the slot pointer must be inside
                         * recv->slots[], so recv (which embeds UCell at
                         * offset 0) is the parent cell. */
                        urbi_gc_slot_write(vm, (UCell *)recv,
                                           (uint32_t)((ic->slots[k] - recv->slots)),
                                           v);
                        *ic->slots[k] = v;
                        slow_path = 0;
                        break;
                    }
                    /* Proto-chain hit (no LOCAL, no OSET, not CONSTANT) →
                     * fall to slow path for COW. */
                    break;
                }
            }
            if (!slow_path) {
                NEXT();
            }

            /* Slow path. */
            int rc = urbi_slot_set_slow(vm, recv, ic, v);
            if (rc != 0) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "SETSLOT: slot write failed (constant, OOM, or resolve overflow)");
                HALT();
            }
            uint8_t fresh_k = (uint8_t)((ic->replace_cursor + URBI_IC_ENTRIES_PER_SITE - 1u)
                                        % URBI_IC_ENTRIES_PER_SITE);
            if (ic->n > 0u && (ic->flags[fresh_k] & URBI_SLOT_FLAG_OSET)) {
                vm->last_error = UVM_TYPE_ERROR;
                vm_format_type_error_msg(vm, "SETSLOT: setter dispatch not yet implemented");
                HALT();
            }
            NEXT();
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
             * T30: allocate a per-scope UTag (no UVAL_TAG / register binding at M3).
             * Each tag-scope gets its own anonymous UTag; the tag's lifetime is
             * bounded by the corresponding OP_POP_TAG.
             * Walker-pop (urbi_unwind via OP_THROW etc.) will leak the UTag at M3 —
             * deferred for T31/walker integration when full tag lifecycle wires through.
             * strand_back = s for future tag.stop() walk (T31 uses). */
            uint8_t  a          = uinstr_a(*s->pc);
            uint8_t  flags      = (uint8_t)((a >> 4) & 0xFu);
            uint16_t handler_pc = uinstr_bx(*s->pc);
            UTag *tag = utag_create(s->vm);
            if (tag == NULL) {
                s->fatal_status = UEXEC_THROW;
                s->state        = USTRAND_STATE_DEAD;
                goto exit_strand;
            }
            UCleanupEntry *entry = strand_cleanup_push(s);
            if (entry == NULL) {
                utag_destroy(s->vm, tag);  /* roll back the tag alloc on overflow */
                s->fatal_status = UEXEC_THROW;
                s->state        = USTRAND_STATE_DEAD;
                goto exit_strand;
            }
            entry->kind           = (uint8_t)UCLEANUP_TAG_SCOPE;
            entry->flags          = flags;
            entry->handler_pc     = handler_pc;
            entry->register_base  = 0u;
            entry->register_count = 0u;
            entry->owning_tag     = tag;
            entry->catch_pattern  = NULL;
            entry->next_member    = tag->member_strands_head;  /* head-insert */
            entry->strand_back    = s;
            tag->member_strands_head = entry;
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
                    /* onleave handler: not reachable at M3 (emit always sets flags=0).
                     * If somehow reached (bytecode corruption), halt safely. */
                    vm->last_error = UVM_TYPE_ERROR;
                    vm_format_type_error_msg(vm, "POP_TAG: FLAG_HAS_ONLEAVE not wired at M3");
                    HALT();
                }
                /* T30: capture owning_tag before pop — the slot remains valid memory but
                 * is below cleanup_depth after pop and may be reused by a later push. */
                UTag *tag = top->owning_tag;
                /* Unlink this entry from tag->member_strands_head (singly-linked
                 * list removal via next_member). Only unlink when tag is non-NULL
                 * — older bytecode emitted before T30 may have owning_tag == NULL. */
                if (tag != NULL) {
                    UCleanupEntry **pp = &tag->member_strands_head;
                    while (*pp != NULL && *pp != top) {
                        pp = &(*pp)->next_member;
                    }
                    if (*pp == top) {
                        *pp = top->next_member;
                    }
                }
                /* Watcher cascade: push each watcher registered on this tag to
                 * the pending-onleave queue before cleanup_pop + utag_destroy.
                 * Snapshot-next iteration since push mutates member_watchers_head
                 * (unlinks the watcher from the tag's member list).
                 * Ordering: cascade BEFORE utag_destroy, which asserts the member
                 * list is empty — push empties it. */
                if (tag != NULL) {
                    UWatcher *ww = tag->member_watchers_head;
                    UWatcher *ww_next;
                    while (ww != NULL) {
                        ww_next = ww->next_in_tag;
                        pending_onleave_queue_push(s->vm, ww);
                        ww = ww_next;
                    }
                }
                strand_cleanup_pop(s, UCLEANUP_TAG_SCOPE);
                /* Destroy the per-scope UTag allocated in OP_PUSH_TAG.
                 * Precondition (checked by utag_destroy assertion): member lists
                 * must be empty — we just unlinked the only member above. */
                if (tag != NULL) {
                    utag_destroy(s->vm, tag);
                }
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
    if (vm->gc_pending)           urbi_gc_slice(vm, URBI_GC_SLICE_BUDGET);
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
    /* Wake any JOIN-blocked parents if this strand just reached DEAD. */
    if (s->state == USTRAND_STATE_DEAD && s->joiners_head != NULL) {
        fork_wake_joiners(s, vm);
    }

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
    strand.vm                   = vm;
    strand.state                = USTRAND_STATE_DORMANT;
    strand.is_uvm_run_transient = 1u;  /* T33: discriminator for OP_FORK_* guards */

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

    /* T33: route this transient onto vm->global_realm->strands_head so the
     * GC realm-hierarchy walker (T32) visits its register window.  Lazy-create
     * the global realm on first use; failure here is non-fatal — the strand
     * stays realm=NULL and the GC walker simply skips it (M3 baseline behavior).
     * The strand stays a stack-local UStrand and is unlinked again before the
     * matching ustrand_destroy below.  Per pre-M4 GC strand-walker spec §5.1.
     * entry_closure stays NULL — that is the discriminator the OP_FORK_DETACH
     * / OP_FORK_JOIN guards now use to reject forks from a uvm_run transient. */
    {
        URealm *gr = urbi_realm_global(vm);
        if (gr != NULL) {
            strand.realm         = gr;
            strand.next_in_realm = gr->strands_head;
            gr->strands_head     = &strand;
        }
    }

    /* Wire frame-0 from module. */
    strand.R          = strand.stack;
    strand.pc         = module->instructions;
    strand.pc_base    = module->instructions;
    strand.cur_consts = module->constants;
    strand.module     = module;
    /* M4 follow-up: bind module_instance for OP_GETSLOT/SETSLOT IC dispatch.
     * urbi_run_chunk already created the UModuleInstance via
     * urbi_get_or_create_module_instance; uvm_run callers (test_vm.c
     * pipeline, test_emit.c integration tests) get the binding here too
     * so OP_CLOSURE can read s->module_instance directly. */
    strand.module_instance = urbi_get_or_create_module_instance(vm, (UModule *)module);
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
        strand.closure_list = NULL;  /* null before ustrand_destroy to avoid double-free */
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
        strand.closed_cells = NULL;  /* null before ustrand_destroy to avoid double-free */
        while (cell != NULL) {
            UUpvalCell *next = cell->next;
            vm->alloc_fn(cell, 0, vm->alloc_ud);
            cell = next;
        }
    }

    /* Free any open upvalue cells still on the strand. */
    vm_free_open_upvalues(vm, &strand);
    strand.open_upvals = NULL;  /* null before ustrand_destroy to avoid double-free */

    /* Free the register stack. */
    vm->alloc_fn(strand.stack, 0, vm->alloc_ud);
    strand.stack = NULL;

    /* T33: unlink the transient from global_realm->strands_head before
     * ustrand_destroy.  The stack-local UStrand is about to leave scope; if
     * we leave it threaded, urealm_teardown_all → urbi_realm_destroy would
     * walk strands_head and call urbi_strand_destroy on a stack address.
     * Symmetric with the head-insert just before frame-0 wiring. */
    if (strand.realm != NULL && strand.realm->strands_head != NULL) {
        UStrand **pp = &strand.realm->strands_head;
        while (*pp != NULL) {
            if (*pp == &strand) {
                *pp = strand.next_in_realm;
                strand.next_in_realm = NULL;
                break;
            }
            pp = &(*pp)->next_in_realm;
        }
        strand.realm = NULL;
    }

    UVMError rc = vm->last_error;
    ustrand_destroy(&strand, vm);
    return rc;
}
