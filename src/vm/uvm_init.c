/* SPDX-License-Identifier: BSD-3-Clause */
/* urbi_vm_init.c — VM lifecycle: init / destroy / native-protos / drain reg.
 * Extracted from uvm.c during v0.5.4-decompose (VM #1). */

/* _POSIX_C_SOURCE must be defined before any system header; guard against
   re-definition in case the build system already defines it. */
#if __STDC_HOSTED__ && (defined(__linux__) || defined(__APPLE__) || defined(__unix__))
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  define UVM_INIT_HAVE_CLOCK_GETTIME 1
#endif

#include "vm/uvm.h"
#include "vm/uvm_internal.h"
#include "runtime/umacros.h"      /* urbi_zero */
#include "runtime/uclosure.h"     /* full UClosure for stdlib_closures teardown */
#include "urbi/urbi.h"            /* URBI_CALLBACK_WARN_US, URBI_WATCHDOG_WARN */
#include "urbi/gc.h"              /* urbi_gc_init, urbi_gc_destroy */
#include "value/uintern.h"        /* uintern_destroy */
#include "sched/ustrand.h"        /* UStrand (forward) */
#include "realm/urealm.h"         /* urealm_teardown_all */
#include "event/uevent_ring.h"    /* UEventRing, uevent_ring_init */
#include "runtime/uhandle.h"      /* host_handle_walk_roots */
#include "watcher/uwatcher.h"     /* uwatcher_pool_init/destroy, watcher_table_walk_roots */
#include "stdlib/containers.h"    /* M6 Phase 6: urbi_stdlib_containers_destroy */
#include "event/uevent_native.h"  /* event_native_register */
#include "tag/utag_native.h"      /* tag_native_register */
#include "object/utypes_init.h"   /* urbi_object_builtin_types_init */
#include "object/uobject.h"       /* urbi_object_register_gc_roots */
#include "sched/usched_cooperative.h" /* sched_walk_roots */
#include "module/umodule.h"           /* umodule_destroy — M6 Phase 4 stdlib_module teardown */

#if __STDC_HOSTED__
#  include <stdlib.h>
#  if defined(UVM_INIT_HAVE_CLOCK_GETTIME)
#    include <time.h>
#  endif
#endif

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
   Embedded callers MUST override via the host_time_us field after urbi_vm_init(). */
static uint64_t default_host_time_us_stub(void) {
#if defined(UVM_INIT_HAVE_CLOCK_GETTIME)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#else
    /* Freestanding or non-POSIX hosted: no clock without platform BSP. */
    return 0U;
#endif
}

void urbi_vm_init(UVM *vm, UVMAllocFn alloc_fn, void *alloc_ud) {
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
    vm->topology_gen   = 1ULL;   /* pre-M4 topology spec §3.1: init=1, 0 reserved */
    vm->lookup_id      = 1ULL;   /* pre-M4 prototype-chain spec §7.1 */
    vm->next_object_id = 0U;     /* pre-M4 prototype-chain spec §8.1 (first alloc → 1) */
    vm->root_shape     = NULL;   /* lazy-allocated by urbi_shape_root */

    /* M4 atom-family singletons (T8) + M6 Phase 4 (Boolean/Nil/Void): all
     * NULL until first lazy-create. */
    vm->atom_object  = NULL;
    vm->atom_integer = NULL;
    vm->atom_float   = NULL;
    vm->atom_string  = NULL;
    vm->atom_list    = NULL;
    vm->atom_dict    = NULL;
    vm->atom_tag     = NULL;
    vm->atom_event   = NULL;
    vm->atom_symbol  = NULL;
    vm->atom_boolean = NULL;
    vm->atom_nil     = NULL;
    vm->atom_void    = NULL;

    /* M5 T53/T54 native proto objects: NULL until event/tag_native_register. */
    vm->event_proto = NULL;
    vm->tag_proto   = NULL;

    /* M4 T30 — UModuleInstance registry head: empty until first
     * urbi_module_instance_create. */
    vm->module_instances_head = NULL;

    vm->last_return_closure  = NULL;

    /* --- M3 field zero-init (rows 8, 9, 10, 11) --- */
    /* 5-flag liveness counters (Rule X). */
    vm->strand_runnable_count   = 0U;
    vm->strand_suspended_count  = 0U;
    vm->watcher_active_count    = 0U;
    vm->event_queue_count       = 0U;
    vm->wakeup_pending_count    = 0U;
    vm->host_call_pending_count = 0U;

    /* Realm / fatal-strand pointers. */
    vm->realms_head  = NULL;
    vm->global_realm = NULL;
    vm->fatal_strand = NULL;
    vm->realm_id_seq = 0U;

    /* Scheduler queues and step-driver state. */
    vm->ready_head             = NULL;
    vm->ready_tail             = NULL;
    vm->sleep_q_head           = NULL;
    vm->step_budget_remaining  = 0U;
    vm->gc_pending             = 0U;
    vm->watcher_dirty_count    = 0U;
    vm->flag_preemption        = 0U;
    vm->flag_reserved[0]       = 0U;
    vm->flag_reserved[1]       = 0U;
    vm->flag_reserved[2]       = 0U;

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
    vm->gc_phase            = 0U;
    vm->current_white       = 0U;
    vm->gc_paused           = 0U;
    vm->in_destroy_callback = 0U;
    vm->gc_live_bytes       = 0U;
    vm->gc_surviving_bytes  = 0U;
    vm->gc_total_allocated  = 0U;
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
    vm->pad_watchdog[0]        = 0U;
    vm->pad_watchdog[1]        = 0U;
    vm->pad_watchdog[2]        = 0U;

    /* Delegate threshold + debt init to the GC strategy (T23). */
    urbi_gc_init(vm);

    /* GC root provider registry: zero before registering providers. */
    {
        uint8_t i;
        for (i = 0U; i < (uint8_t)URBI_MAX_ROOT_PROVIDERS; i++) {
            vm->root_providers[i] = NULL;
        }
    }
    vm->root_provider_count = 0U;

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
    vm->host_type_count      = 0U;

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

    /* T53/T54: event_native_register + tag_native_register allocate UObject
     * proto cells and intern slot-name strings.  They are NOT called here
     * because existing GC + intern + object-model tests assert on exact cell /
     * entry counts immediately after urbi_vm_init (the atom singletons themselves
     * are lazy for the same reason).  Callers that need the native protos must
     * call urbi_native_protos_init(vm) after urbi_vm_init — or test them via the
     * typed C helpers (tag_enter_getter / tag_leave_getter) directly.
     *
     * The full "call from VM init" wiring will land when the globals-exposure
     * task (T59) makes Event/Tag resolvable by name, at which point the cell
     * counts in the affected unit tests will be updated in the same commit. */

    vm->handle_table         = NULL;
    vm->handle_table_cap     = 0U;
    vm->handle_table_next_id = 0U;

    /* Watcher pool. */
    vm->watcher_pool_base      = NULL;
    vm->watcher_pool_freelist  = NULL;
    vm->active_watchers_head   = NULL;
    vm->watcher_pool_in_use    = 0U;
    vm->watcher_pool_high_water = 0U;
    vm->in_watcher_eval        = 0U;
    vm->pad_in_eval[0]         = 0U;
    vm->pad_in_eval[1]         = 0U;   /* array is [2]; index 2 removed */
    /* Zero-initialize the in_watcher_scratch re-entrancy guard so the
     * dispatcher's slow-path detection in c_event_emit_sync /
     * c_event_waituntil starts from the correct "not currently inside a
     * scratch-frame body" state on a fresh VM (spec #3 §5.4). */
    vm->in_watcher_scratch     = 0U;
    /* spec #2 §5.2 install-time trace state. */
    vm->in_watcher_install     = 0U;
    vm->trace_overflow         = 0U;
    vm->trace_read_set_count   = 0U;
    /* trace_read_set[] is uninitialized: only read when in_watcher_install is set,
     * and entries are written before they are read. */
    vm->test_watcher_condition_hook = NULL;
    vm->test_watcher_fire_hook      = NULL;
    vm->test_watcher_onleave_hook   = NULL;
    vm->test_install_cond_hook      = NULL;
    vm->pending_onleave_head   = NULL;
    vm->pending_onleave_tail   = NULL;

    /* spec #3 §7.1: cur_strand is set by urbi_step around dispatch and
     * cleared after.  Must be NULL on a fresh VM so callers (e.g. the
     * EVENT-022 step-quiescent assert in urbi_register_event_drain) can
     * trust it as the canonical "step in progress" signal. */
    vm->cur_strand             = NULL;

    /* Watcher pool: allocate after field zero-init and after GC init
     * so pool_alloc can use vm->current_white and vm->alloc_fn is set. */
    uwatcher_pool_init(vm);
    /* OOM note: if uwatcher_pool_init returns -1, watcher_pool_base stays NULL.
     * pool_alloc will return NULL on any install attempt — still safe. Matches the
     * event_ring OOM pattern from row 9 scheduler work: silently leaves the pointer
     * NULL; the install API returns NULL on first use, surfacing the failure at the
     * use site rather than at vm_init. The embedded caller should check
     * vm->watcher_pool_base != NULL post-init. */

    /* Deferred slot-change ring (spec #4 §3.5): one allocation per VM. */
    vm->slot_change_reentrancy_warned = 0U;
    vm->slot_change_ring_full_warned  = 0U;
    vm->event_sync_degradation_warned = 0U;  /* EMITR-005 one-shot flag */
    vm->deferred_slot_changes_head    = 0U;
    vm->deferred_slot_changes_tail    = 0U;
    vm->deferred_slot_changes_cap     = 0U;
    vm->deferred_slot_changes         = NULL;
    if (vm->alloc_fn) {
        size_t ring_bytes = (size_t)URBI_DEFERRED_SLOT_CHANGE_RING_SIZE
                            * sizeof(UDeferredSlotChange);
        UDeferredSlotChange *ring = (UDeferredSlotChange *)vm->alloc_fn(
                NULL, ring_bytes, vm->alloc_ud);
        if (ring != NULL) {
            /* Zero-fill via volatile byte loop (freestanding: no memset). */
            urbi_zero(ring, ring_bytes);
            vm->deferred_slot_changes     = ring;
            vm->deferred_slot_changes_cap = (uint16_t)URBI_DEFERRED_SLOT_CHANGE_RING_SIZE;
        }
        /* OOM: leave deferred_slot_changes NULL; drain/enqueue guards against it. */
    }

    /* Host time hook: default stub; embedded callers override post-init. */
    vm->host_time_us = default_host_time_us_stub;

    /* T57: ISR drain handler (spec #3 §9): NULL until host registers one. */
    vm->event_drain_handler = NULL;

    /* M6 Phase 3: stdlib state. */
    vm->stdlib_closures        = NULL;
    vm->stdlib_upvalues        = NULL;
    vm->stdlib_module          = NULL;   /* M6 Phase 4: lazy-allocated by urbi_stdlib_boot */
    vm->stdlib_containers      = NULL;   /* M6 Phase 6: backing-buffer head; populated by container .new() bodies */
    vm->container_pair_proto    = NULL;  /* M6 Phase 6 — populated by urbi_stdlib_register_containers */
    vm->container_triplet_proto = NULL;
    vm->container_tuple_proto   = NULL;
    vm->exception_proto         = NULL;  /* M6 Phase 7 — populated by urbi_stdlib_register_runtime_types */
    vm->math_proto              = NULL;  /* M6 Phase 8 T86 — populated by urbi_stdlib_register_namespaces */
    vm->system_proto            = NULL;  /* M6 Phase 8 T87 */
    vm->platform_proto          = NULL;  /* M6 Phase 8 T88 */
    vm->stdlib_booted          = 0U;
    {
        int i;
        for (i = 0; i < 7; i++) vm->pad_stdlib[i] = 0U;
    }
    vm->last_recv = urbi_value_nil();
}

void urbi_vm_destroy(UVM *vm) {
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

    /* Free deferred slot-change ring (spec #4 §3.5). */
    if (vm->deferred_slot_changes != NULL && vm->alloc_fn != NULL) {
        vm->alloc_fn(vm->deferred_slot_changes, 0, vm->alloc_ud);
        vm->deferred_slot_changes = NULL;
    }

    /* Free heap fields owned by the embedder-callable VM lifecycle.
     * handle_table is allocated lazily on first use; free here if non-NULL. */
    if (vm->handle_table != NULL && vm->alloc_fn != NULL) {
        vm->alloc_fn(vm->handle_table, 0, vm->alloc_ud);
        vm->handle_table = NULL;
    }

    /* M2 baseline teardown. */
    uintern_destroy(vm);
    /* M6 Phase 3: clear last_return_closure pointer.  Pre-Phase 3 this
     * call freed the closure directly; Phase 3 migrates run-end closures
     * onto vm->stdlib_closures (see uvm_run.c) so the closure gets reclaimed
     * by the stdlib_closures sweep below.  Clearing the field guards
     * against accidental dereference after destroy without the
     * extra free that would now be a double-free. */
    vm->last_return_closure = NULL;
    /* M6 Phase 3: free vm-lifetime UClosures (both native stdlib closures
     * registered by urbi_native_closure_create AND user closures migrated
     * from strand closure_lists at run exit, see uvm_run.c).  All threaded
     * via next_alloc on a single vm->stdlib_closures list. */
    if (vm->alloc_fn != NULL) {
        UClosure *cl = vm->stdlib_closures;
        while (cl != NULL) {
            UClosure *next = cl->next_alloc;
            vm->alloc_fn(cl, 0, vm->alloc_ud);
            cl = next;
        }
        vm->stdlib_closures = NULL;

        /* M6 Phase 3: free vm-lifetime heapified upvals (UUpvalCells
         * migrated from strand closed_cells at run exit).  These must
         * outlive their owning closures, which are also on
         * stdlib_closures above. */
        UUpvalCell *uc = vm->stdlib_upvalues;
        while (uc != NULL) {
            UUpvalCell *next = uc->next;
            vm->alloc_fn(uc, 0, vm->alloc_ud);
            uc = next;
        }
        vm->stdlib_upvalues = NULL;

        /* M6 Phase 4 (Wave 2): free the heap-allocated stdlib UModule
         * deserialized at boot.  Runs AFTER urbi_gc_destroy above so any
         * UModuleInstance referencing this module has already been
         * reaped — no dangling ic_names back-reference can survive. */
        if (vm->stdlib_module != NULL) {
            umodule_destroy(vm->stdlib_module);
            vm->alloc_fn(vm->stdlib_module, 0, vm->alloc_ud);
            vm->stdlib_module = NULL;
        }

        /* M6 Phase 6: free container backing buffers (List/Dict/Tuple
         * storage) threaded onto vm->stdlib_containers.  Runs after
         * urbi_gc_destroy so the per-instance UObjects (which carry the
         * pointer in a hidden _storage slot) have already been reaped —
         * no dangling buffer reference can survive. */
        urbi_stdlib_containers_destroy(vm);
    }
    /* Note: open_upvals is now on the strand, not the VM.
       The urbi_vm_run adapter cleans up strand.open_upvals before destroy. */
}

/* urbi_native_protos_init: allocate vm->event_proto + vm->tag_proto and
 * install their native slots.  Must be called after urbi_vm_init.
 *
 * Separated from urbi_vm_init because existing unit tests that assert exact
 * cell / intern counts immediately post-init would break (atoms are lazy
 * for the same reason).  The T59 globals-exposure task will wire this into
 * the vm-create path once the affected tests are updated to account for the
 * additional cells. */
void
urbi_native_protos_init(UVM *vm)
{
    /* Propagate UVM_OOM via vm->last_error so callers can detect failure.
     * Both event_native_register and tag_native_register (TAGCH-004) now
     * return UVMError so partial-init OOM is surfaced rather than silently
     * leaving NULL protos behind. */
    UVMError err = event_native_register(vm);
    if (err != UVM_OK) {
        vm->last_error = err;
        return;
    }
    err = tag_native_register(vm);
    if (err != UVM_OK) {
        vm->last_error = err;
        return;
    }
}

/* === urbi_register_event_drain (T57 — spec #3 §9) ===
 *
 * Install a host callback that is invoked at each safepoint (urbi_step entry)
 * for every entry drained from the ISR SPSC ring.  The handler maps event_id
 * to a UEvent* and typically calls c_event_emit_async.  Pass NULL to remove
 * the handler.  Not ISR-safe: must be called from the same thread as urbi_step.
 *
 * Step-safety contract (EVENT-022): only call between urbi_step / urbi_vm_run
 * slices, never from inside one.  Concretely: vm->cur_strand must be NULL.
 * A non-NULL cur_strand means a strand is currently dispatching, in which
 * case the drain handler is being read concurrently by uevent_ring_drain
 * (called at safepoint entry).  Today's single-threaded scheduler means the
 * "concurrent" reader is on the same thread as us, so a write here would not
 * race in the SMP sense — but it would mutate the handler mid-step, which
 * is observably surprising semantics, and the v1.x URBI_SCHED_PREEMPTIVE
 * design assumes drain-handler installs are quiescent.  URBI_DEBUG asserts
 * the contract.
 */
void
urbi_register_event_drain(UVM *vm, urbi_event_drain_handler h)
{
    /* API-003: NULL check FIRST.  URBI_ASSERT_NOT_ISR expands to call
     * urbi_in_isr(vm) which is itself NULL-safe, so the prior order was
     * not strictly buggy — but the convention across the public surface
     * is "validate args, then assert invariants", and reading the code
     * top-down was misleading. */
    if (vm == NULL) return;
    URBI_ASSERT_NOT_ISR(vm);
    /* EVENT-022: enforce the step-quiescent contract above.  cur_strand
     * is set by urbi_step before dispatch and cleared afterwards; it is
     * the canonical "step in progress" signal in this VM (no separate
     * in_step flag exists). */
    URBI_INTERNAL_ASSERT(vm->cur_strand == NULL);
    /* EVENT-007: __ATOMIC_RELEASE store pairs with the __ATOMIC_ACQUIRE
     * load in uevent_ring_drain.  Single-threaded today; the pairing
     * inherits correctness for v1.x URBI_SCHED_PREEMPTIVE. */
    __atomic_store_n(&vm->event_drain_handler, h, __ATOMIC_RELEASE);
}

const char *uvm_error_name(UVMError code) {
    switch (code) {
        case UVM_OK:         return "UVM_OK";
        case UVM_TYPE_ERROR: return "UVM_TYPE_ERROR";
        case UVM_OOM:        return "UVM_OOM";
    }
    return "UVM_UNKNOWN";
}
