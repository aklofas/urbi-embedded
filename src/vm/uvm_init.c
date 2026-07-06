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
#include "vm/uvm_ref.h"           /* urbi_gc_ref_table_walk_roots */
#include "runtime/umacros.h"      /* urbi_zero */
/* uclosure.h include removed at v0.8.4 Step C-3 (stdlib_closures teardown deleted). */
#include "urbi/urbi.h"            /* URBI_CALLBACK_WARN_US, URBI_WATCHDOG_WARN */
#include "urbi/gc.h"              /* urbi_gc_init, urbi_gc_destroy */
#include "gc/ugc_incremental.h"   /* urbi_gc_shade_gray (vm_misc_walk_roots Step C-1) */
#include "value/uintern.h"        /* uintern_destroy */
#include "sched/ustrand.h"        /* UStrand (forward) */
#include "realm/urealm.h"         /* urealm_teardown_all */
#include "event/uevent_ring.h"    /* UEventRing, uevent_ring_init */
#include "runtime/uhandle.h"      /* urbi_gc_host_handle_walk_roots */
#include "watcher/uwatcher.h"     /* uwatcher_pool_init/destroy, urbi_gc_watcher_table_walk_roots */
#include "watcher/uwatcher_state.h" /* uwatcher_state_create/destroy (W2/v0.10.4) */
#include "stdlib/temporal.h"      /* urbi_periodic_table_walk_roots, urbi_periodic_destroy_all */
#include "stdlib/containers.h"    /* M6 Phase 6: urbi_stdlib_containers_destroy */
#include "event/uevent_native.h"  /* urbi_event_native_register */
#include "event/uevent_registry.h" /* uevent_registry_init, uevent_registry_destroy */
#include "tag/utag_native.h"      /* urbi_tag_native_register */
#include "object/utypes_init.h"   /* urbi_object_builtin_types_init */
#include "object/uobject.h"       /* urbi_object_register_gc_roots */
#include "sched/usched_cooperative.h" /* urbi_gc_sched_walk_roots */
#include "chunk/uchunk.h"           /* uchunk_destroy — M6 Phase 4 stdlib_module teardown */
#include "urbi/types.h"               /* URBI_OK, URBI_ERR_OOM — T23 return-code surface */
#include "changed/uchanged_node.h"  /* urbi_deferred_slot_changes_walk_roots */
#include "runtime/utest_hooks.h"    /* W3/v0.10.4: UTestHooks lifecycle */
#ifdef URBI_ENABLE_ROS2
#include "urbi/ros.h"               /* urbi_ros_shutdown — VM-scope the ros bridge */
#endif
#if URBI_ENABLE_REPL
#  include "repl/urepl_state.h"     /* W3/v0.10.4: UReplState lifecycle (destroy in vm teardown) */
#endif

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
   Embedded callers MUST override via urbi_set_clock_fn() after urbi_vm_init().
   urbi_default_host_time_us is the non-static alias used by uvm_writer.c so
   that urbi_set_clock_fn(vm, NULL) can restore the built-in default without
   duplicating the #ifdef logic. */
/* v0.10.3 (W3): signature gains void *ud (urbi_time_us_fn convention). */
static uint64_t default_host_time_us_stub(void *ud) {
    (void)ud;
#if defined(UVM_INIT_HAVE_CLOCK_GETTIME)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#else
    /* Freestanding or non-POSIX hosted: no clock without platform BSP. */
    return 0U;
#endif
}

/* Non-static alias: lets uvm_writer.c restore the built-in time source. */
uint64_t urbi_default_host_time_us(void *ud) {
    return default_host_time_us_stub(ud);
}

/* === vm_misc_walk_roots (v0.8.4 Option B Step C-1) ===
 *
 * GC root provider for VM-level state that doesn't fit the realm / strand /
 * watcher / intern-table / host-handle / object-proto categories.  Currently
 * yields vm->last_return_closure (the result of the most-recent urbi_vm_run
 * call, preserved across calls for host inspection).
 *
 * UClosure is GC-managed (enrolled via urbi_gc_alloc since v0.8.4 Step C-2),
 * so this yield is load-bearing: it is what keeps the preserved closure
 * alive across collections until the next urbi_vm_run replaces it.
 *
 * v0.13.2: also walks vm->c_roots_head — the VM-level C-stack root frame
 * chain (strandless counterpart of UStrand.c_roots_head; see uvm.h field
 * comment and urbi_c_root_push in gc/ugc_incremental.c). */
static void
vm_misc_walk_roots(UVM *vm, UGcRootCallback cb, void *ctx)
{
    if (vm->last_return_closure != NULL) {
        urbi_gc_shade_gray(vm, (UCell *)&vm->last_return_closure->cell);
    }
    for (const UCRootFrame *f = vm->c_roots_head; f != NULL; f = f->next) {
        cb(vm, f->slot, ctx);
    }
}

int urbi_vm_init(UVM *vm, UVMAllocFn alloc_fn, void *alloc_ud) {
    /* T23 (VM-010 + VM-024): promoted from void to int return.  Sub-system
     * allocations that can OOM now surface URBI_ERR_OOM; callers can detect
     * partial init.  urbi_vm_destroy stays safe on any partial-init state
     * reached before the bailout. */
    int oom_seen = 0;
    /* Backstop zero-fill: every UVM field is 0/NULL before sub-system inits
     * run.  The volatile loop (urbi_zero, not memset) is required for
     * freestanding builds — it defeats dead-store elimination on caller-
     * provided memory (e.g. UVM stack-allocated in tests).
     *
     * Fields that need a non-zero initial value are assigned explicitly
     * below.  The ordering comments that follow describe WHICH fields the
     * subsystem depends on; the actual zeroing is done here once. */
    urbi_zero(vm, sizeof(*vm));

#ifdef URBI_DEBUG
    /* v0.10.1 W4: notify the refcount accounting layer that a new VM is alive.
     * Paired with urbi_proto_ref_vm_gone() in urbi_vm_destroy. */
    urbi_proto_ref_vm_born();
#endif

    /* v0.9.1 / W3-v0.10.4 — vm->repl must be NULL before any subsystem init
     * that drives bytecode (urbi_run_chunk → urbi_step →
     * urepl_dispatch_drain_if_active reads this field).  vm->debug_proto must
     * be NULL before object_roots_walker runs (walker dereferences it if
     * non-NULL).  Both are covered by the urbi_zero backstop above; this
     * comment is the preserved ordering exemplar for stack-allocated UVMs. */

    vm->alloc_fn = alloc_fn;
    vm->alloc_ud = alloc_ud;
#if __STDC_HOSTED__
    if (vm->alloc_fn == NULL) {
        vm->alloc_fn = uvm_stdlib_realloc;
        vm->alloc_ud = NULL;
    }
#endif
#if URBI_TRACE
    /* v0.11.0: heap-allocate trace state now that alloc_fn is finalized, and
     * before any subsystem init that could fire a tracepoint (stdlib boot
     * drives the scheduler/GC). NULL-on-OOM ⇒ trace simply stays disabled. */
    urbi_trace_init(vm);
#endif
    vm->topology_gen   = 1ULL;   /* pre-M4 topology spec §3.1: init=1, 0 reserved */
    vm->lookup_id      = 1ULL;   /* pre-M4 prototype-chain spec §7.1 */

    /* ISR ring: allocate and initialise the SPSC event ring. */
    if (vm->alloc_fn) {
        vm->event_ring = (struct UEventRing *)vm->alloc_fn(
                NULL, sizeof(UEventRing), vm->alloc_ud);
        if (vm->event_ring) {
            uevent_ring_init(vm->event_ring);
        } else {
            /* T23: surface partial-init OOM to caller via URBI_ERR_OOM.
             * inject/drain still guard against NULL event_ring so the
             * remainder of init runs without aborting; destroy is safe. */
            oom_seen = 1;
        }
    }

    /* GC state machine.
     * gc_phase = 0 = IDLE per row 10 §6.2; named constant lands at T22.
     * (GC fields zeroed by urbi_zero backstop above.) */
#if URBI_PERF_COUNTERS
    urbi_perf_reset(vm);
#endif

    /* T19 ISR-check + debug watchdog hooks: initialize before any subsystem
     * calls URBI_ASSERT_NOT_ISR (T23 onwards). */
    vm->callback_warn_us       = URBI_CALLBACK_WARN_US;
    vm->callback_watchdog_mode = URBI_WATCHDOG_WARN;

    /* Delegate threshold + debt init to the GC strategy (T23). */
    urbi_gc_init(vm);

    /* Register default root providers.
     * Order: scheduler, realm, intern, host-handle, vm-misc, watcher table. */
    urbi_gc_register_root_provider(vm, urbi_gc_sched_walk_roots);
    urbi_gc_register_root_provider(vm, urbi_gc_realm_list_walk_roots);
    urbi_gc_register_root_provider(vm, urbi_gc_intern_table_walk_roots);
    urbi_gc_register_root_provider(vm, urbi_gc_host_handle_walk_roots);
    urbi_gc_register_root_provider(vm, vm_misc_walk_roots);   /* Step C-1 */
    urbi_gc_register_root_provider(vm, urbi_gc_watcher_table_walk_roots);
    /* v0.9.4 Phase 5: every() periodic-spawn primitive — yields each
     * UPeriodic.body closure + vm->every_native_closure to the GC mark. */
    urbi_gc_register_root_provider(vm, urbi_periodic_table_walk_roots);
    /* W3/v0.10.2: deferred slot-change ring (reactive audit F6).
     * Makes vm->deferred_slot_changes[head..tail] visible to GC root walking.
     * No-op on empty ring; under the cooperative scheduler this is
     * correctness-preserving.  Becomes load-bearing at v1.x preemption. */
    urbi_gc_register_root_provider(vm, urbi_deferred_slot_changes_walk_roots);
    /* refactor-3 B2/GC-01/STD-01: stdlib container backing-buffer elements
     * (UList items[] / UDict entries[]) — invisible to the object walker
     * because the script-visible `_storage` slot is a UVAL_INT leaf. */
    urbi_gc_register_root_provider(vm, urbi_stdlib_containers_walk_roots);
    /* Provider headroom: 11 of URBI_MAX_ROOT_PROVIDERS (12) slots used —
     * the 9 above + urbi_gc_ref_table_walk_roots (below) + object_roots_walker
     * (urbi_object_register_gc_roots).  v0.13.2 rewrites the watcher
     * provider IN PLACE (count stays 11). */

    /* Type table + host-handle table.
     * (type_table[] and host_type_count zeroed by urbi_zero backstop.) */

    /* Register built-in M4 object-model UType descriptors directly into
     * vm->type_table[].  Built-in tags can't go through urbi_register_type
     * (which guards tags < UTYPE_HOST_BASE per src/utype.c). */
    urbi_object_builtin_types_init(vm);

    /* T36: register the M4 GC root provider for atom singletons +
     * vm->root_shape + the UChunkInstance chain.  Replaces the manual
     * urbi_pin calls on atom singletons that lived in T8.  Must come
     * after the type-table setup so the walker's urbi_gc_shade_gray invocations
     * find a registered UType for each cell. */
    urbi_object_register_gc_roots(vm);

    /* T53/T54: urbi_event_native_register + urbi_tag_native_register allocate UObject
     * proto cells and intern slot-name strings.  They are NOT called here
     * because existing GC + intern + object-model tests assert on exact cell /
     * entry counts immediately after urbi_vm_init (the atom singletons themselves
     * are lazy for the same reason).  Callers that need the native protos must
     * call urbi_native_protos_init(vm) after urbi_vm_init — or test them via the
     * typed C helpers (urbi_tag_enter_getter / urbi_tag_leave_getter) directly.
     *
     * The full "call from VM init" wiring will land when the globals-exposure
     * task (T59) makes Event/Tag resolvable by name, at which point the cell
     * counts in the affected unit tests will be updated in the same commit. */

    /* (handle_table, watchers, trace, cur_strand zeroed by urbi_zero backstop.) */

    /* W2+W3/v0.10.4 substate allocations.
     *
     * Watcher substate (W2/v0.10.4): UWatcherState struct + pool storage.
     * Pool storage is allocated separately by uwatcher_pool_init below.
     * The struct itself is heap-allocated; uwatcher_state_create zero-fills it.
     *
     * Test-seam substate (W3/v0.10.4): UTestHooks struct, allocated below
     * after the allocator is wired so alloc_fn is available. */
    /* trace_read_set[] is only read when watchers->in_install is set;
     * entries are written before they are read (covered by urbi_zero). */
    {
        UWatcherState *ws = uwatcher_state_create(vm);
        if (ws == NULL) {
            oom_seen = 1;
        }
        vm->watchers = ws;
    }

    /* spec #3 §7.1: cur_strand must be NULL so callers (e.g. the
     * EVENT-022 step-quiescent assert in urbi_register_event_drain) can
     * trust it as the canonical "step in progress" signal.
     * (Covered by urbi_zero backstop.) */

    /* Watcher pool: allocate after field zero-init and after GC init
     * so pool_alloc can use vm->current_white and vm->alloc_fn is set.
     * uwatcher_pool_init writes vm->watchers->pool_base / pool_freelist. */
    if (uwatcher_pool_init(vm) != 0) {
        /* T23: surface OOM.  pool_alloc still returns NULL at use sites,
         * so the install path remains safe even if we did not bail.
         * Pre-T23 this was a silent leave-NULL. */
        oom_seen = 1;
    }

    /* Deferred slot-change ring (spec #4 §3.5): one allocation per VM.
     * (Head/tail/cap/warnings zeroed by urbi_zero backstop.) */
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
        } else {
            /* T23: surface partial-init OOM.  drain/enqueue still guard
             * against NULL so the remainder of init runs without aborting. */
            oom_seen = 1;
        }
    }

    /* Host time hook: default stub; embedded callers override post-init. */
    vm->host_time_us = default_host_time_us_stub;
    /* (host_time_ud, writer_fn/ud, wake_fn/ud, event_drain_handler/ud,
     * stdlib protos, atomic_active, error_ring, ref_table scalars — all
     * zeroed by urbi_zero backstop above.) */

    /* M6 Phase 3: stdlib state. */
    /* (All stdlib proto pointers zeroed by urbi_zero backstop above.
     * stdlib_closures + stdlib_upvalues deleted at v0.8.4 Step C-3.
     * stdlib_protos + stdlib_nested_arrays deleted at Task 11.) */

    /* Gap R (v0.7.1): atomic_active must be zero so uevent_ring_drain is
     * NOT gated on entry.  Covered by urbi_zero backstop above. */

    /* Gap B (v0.7.1): named-event registry.
     * uevent_registry_init zeroes entries/count/capacity/next_id. */
    uevent_registry_init(&vm->event_registry);

    /* Gap J (v0.7.1): host-side watcher table.
     * next_handle must start at 1 (0 = URBI_WATCHER_HANDLE_INVALID). */
    uhost_watcher_table_init(&vm->host_watcher_table);

    /* Gap Q (v0.7.1): reference table — heap-allocated lazily on first urbi_ref.
     * free_list_head sentinel is non-zero (SIZE_MAX); other fields are zero.
     * Register the GC root walker so pinned values survive collection. */
    vm->ref_table.free_list_head = (size_t)-1;  /* SIZE_MAX: no free slots */
    urbi_gc_register_root_provider(vm, urbi_gc_ref_table_walk_roots);

    /* vm->repl is zeroed by urbi_zero backstop (the step-driver hook reads it
     * on every urbi_step call).  REPL state is allocated by urepl_state_create
     * only when a server is started (W3/v0.10.4). */

    /* W3/v0.10.4: allocate the UTestHooks wrapper so tests can install
     * watcher/install seams via vm->test_hooks->watcher_condition etc.
     * vm->test_hooks is NULL-checked at every use site so freestanding
     * builds (alloc_fn == NULL) are safe. */
    if (vm->alloc_fn != NULL) {
        vm->test_hooks = utest_hooks_create(vm);
        if (vm->test_hooks == NULL) {
            oom_seen = 1;
        }
    }

    /* Gap #4 (M6 Wave 3): heap-allocate the operator-overload IC table.
     * Keeps UVM stack-allocation safe (tests that do `UVM vm;` on the C
     * stack would overflow with a 4 KB inline IC). */
    if (vm->alloc_fn != NULL) {
        UOpOverloadIC *ic = (UOpOverloadIC *)vm->alloc_fn(
                NULL, sizeof(UOpOverloadIC), vm->alloc_ud);
        if (ic != NULL) {
            urbi_zero(ic, sizeof(UOpOverloadIC));
            vm->op_overload_ic = ic;
        } else {
            /* T23: surface partial-init OOM.  Fallback helpers in
             * uvm_op_overload.c guard against NULL so dispatch survives. */
            oom_seen = 1;
        }
    }

    /* refactor-3 TEST-GAP-01: arm GC stress mode only now — every root
     * provider is registered, so anything allocated after this point
     * (native protos, stdlib boot, user code — all of which run after
     * urbi_vm_init returns) must survive via a registered root.  Success
     * path only: partial-init (OOM) VMs stay un-armed. */
    if (!oom_seen) {
        vm->gc_stress_armed = 1U;
    }

    /* T23 (VM-010 + VM-024): single return point for the success/OOM
     * decision.  The destroy path is safe to call on partial-init state,
     * so we let every sub-system init run before reporting OOM (this keeps
     * the use-site guards live and exercises them in the OOM-coverage
     * tests). */
    return oom_seen ? URBI_ERR_OOM : URBI_OK;
}

void urbi_vm_destroy(UVM *vm) {
    if (vm == NULL) return;

#ifdef URBI_ENABLE_ROS2
    /* Tear down the process-global ROS bridge if this VM owns it, before the
     * VM heap is reclaimed below — so the dangling UEvent* are dropped and the
     * mock transport allocation is freed (not leaked) while alloc_fn is live. */
    urbi_ros_shutdown(vm);
#endif

#if URBI_TRACE
    /* v0.11.0: free heap-allocated trace state (paired with urbi_trace_init). */
    urbi_trace_destroy(vm);
#endif

    /* --- M3 teardown stubs (in reverse-init order) ---
     * Subsystem-owned teardowns are deferred to their landing tasks. */
    urealm_teardown_all(vm);  /* T14: destroy all live Realms */
    uwatcher_pool_destroy(vm);  /* T32: free pool slab before GC */
    /* W2/v0.10.4: free UWatcherState struct after pool slab is freed. */
    uwatcher_state_destroy(vm, vm->watchers);
    vm->watchers = NULL;
    /* v0.9.4: free any periodics left dangling after realm teardown.
     * urbi_realm_destroy marks per-realm periodics for unregister but
     * (intentionally) doesn't unlink-and-free them — that work is the
     * pump sweep's job, and if no urbi_step ran between realm-destroy
     * and vm-destroy the records would leak.  Drain explicitly here. */
    urbi_periodic_destroy_all(vm);
    /* Clear module_instances_head before GC destroy so that:
     *   (a) object_roots_walker stops shading now-unreachable UChunkInstance
     *       cells (harmless but tidy), and
     *   (b) uchunk_destroy_internal's vm->module_instances_head walk (Task 10)
     *       skips the list instead of dereferencing GC-freed cells post-destroy.
     * The GC sweep will reclaim all UChunkInstance cells regardless; we only
     * clear the pointer so the walk in the stdlib teardown path below is safe. */
    vm->module_instances_head = NULL;
    /* GC destroy must run after all subsystems that hold GC-managed cells.
     * Realm teardown (above) releases bindings; remaining infrastructure (event ring,
     * sched queues) is freed below — none of it owns GC cells. */
    urbi_gc_destroy(vm);      /* T23: free all GC-managed cells */
#if URBI_MEM_DEBUG
    /* v0.11.3: after GC has swept all cells into the quarantine, flush + verify
     * the quarantine poison and free the substate.  Runs while vm->alloc_fn is
     * still live (event ring etc. are freed below). */
    umemdbg_destroy(vm);
#endif
#ifdef URBI_DEBUG
    /* v0.10.1 W4: decrement the active-VM count; fires urbi_proto_ref_assert_balanced()
     * when the last VM is destroyed.  Runs after urealm_teardown_all (all strands
     * destroyed → strand-bind refs released) and urbi_gc_destroy (all UClosure cells
     * finalized → closure-bind refs released).  The balanced check only fires when
     * zero VMs remain so multi-VM test scenarios do not produce false positives. */
    urbi_proto_ref_vm_gone();
#endif
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

    /* W3/v0.10.4: free UTestHooks wrapper (audit-1 F8). */
    if (vm->test_hooks != NULL) {
        utest_hooks_destroy(vm, vm->test_hooks);
        vm->test_hooks = NULL;
    }

#if URBI_ENABLE_REPL
    /* W3/v0.10.4: free UReplState wrapper if allocated (embedder skipped
     * urbi_repl_stop).  Pre-W3 vm->repl_server was an inline void* (no heap
     * allocation); W3 introduced an 8-byte heap wrapper that leaked when
     * urbi_vm_destroy ran without a preceding urbi_repl_stop.
     * urepl_state_destroy is NULL-tolerant; VMs that never started REPL have
     * vm->repl == NULL and this is a no-op.  Must run before alloc_fn is
     * freed (urepl_state_destroy calls vm->alloc_fn). */
    urepl_state_destroy(vm, vm->repl);
    vm->repl = NULL;
#endif

    /* Gap #4 (M6 Wave 3): free heap-allocated operator-overload IC. */
    if (vm->op_overload_ic != NULL && vm->alloc_fn != NULL) {
        vm->alloc_fn(vm->op_overload_ic, 0, vm->alloc_ud);
        vm->op_overload_ic = NULL;
    }

    /* Gap B (v0.7.1): free named-event registry entries[] array.
     * Must run after urbi_gc_destroy (above) so GC has already reaped any
     * UEvent cells; the registry only held raw (non-owning) pointers to them.
     * Interned name strings are freed by uintern_destroy (below). */
    uevent_registry_destroy(&vm->event_registry, vm);

    /* Gap J (v0.7.1): free host-watcher table entries[] array. */
    uhost_watcher_table_destroy(&vm->host_watcher_table, vm);

    /* Gap P (v0.7.1): error ring — inline storage; no heap to free. */

    /* Gap Q (v0.7.1): reference table — free heap-allocated slots array. */
    if (vm->ref_table.slots != NULL && vm->alloc_fn != NULL) {
        vm->alloc_fn(vm->ref_table.slots, 0, vm->alloc_ud);
        vm->ref_table.slots    = NULL;
        vm->ref_table.capacity = 0U;
    }

    /* M2 baseline teardown. */
    uintern_destroy(vm);
    /* v0.8.4 Step C-3: stdlib_closures + stdlib_upvalues fields deleted.
     * UClosure + UUpvalCell are GC-managed; urbi_gc_destroy (called above)
     * already swept all white cells and invoked the uclosure_destroy finalizer
     * on each closure.  vm->last_return_closure: GC-managed; cleared for
     * post-destroy hygiene (the closure was already reclaimed above). */
    vm->last_return_closure = NULL;

    if (vm->alloc_fn != NULL) {

        /* M6 Phase 4 (Wave 2): free the heap-allocated stdlib root UProto
         * deserialized at boot.  Runs AFTER urbi_gc_destroy above so any
         * UChunkInstance referencing this root has already been
         * reaped — no dangling ic_names back-reference can survive.
         *
         * Ordering: BEFORE rescued_protos sweep below.  uchunk_destroy may
         * rescue a non-zero-refcount root onto vm->rescued_protos; that
         * rescued proto is freed by the sweep that follows.
         * v0.9.2: uchunk_destroy frees heap_allocated roots automatically. */
        if (vm->stdlib_module != NULL) {
            uchunk_destroy(vm->stdlib_module, vm);
            /* uchunk_destroy freed the struct (heap_allocated=true); clear ptr. */
            vm->stdlib_module = NULL;
        }
#ifdef URBI_ENABLE_UROBOTICS
        /* v0.12.2: VM-owned Robotics overlay module — freed exactly like
         * stdlib_module, before the rescued_protos sweep below so any rescued
         * protos land correctly. */
        if (vm->urobotics_module != NULL) {
            uchunk_destroy(vm->urobotics_module, vm);
            vm->urobotics_module = NULL;
        }
#endif

        /* Task 11 (v0.8.1-uproto-root): stdlib_protos and stdlib_nested_arrays
         * deleted.  The rescued_protos sweep below handles all deferred protos. */

        /* Phase 2 Task 9 (v0.8.1-uproto-root): free rescued whole root_protos.
         * Each entry is a root_proto that was detached from its UModule by
         * uchunk_destroy when root_proto->refcount > 0 (strand still alive).
         * The root_proto carries ownership of nested[] and all chunk-top buffers
         * (module shell was freed normally with those fields NULLed).
         *
         * Ordering: run AFTER stdlib_module destroy (above) since that may
         * rescue protos here.  Must run BEFORE the VM allocator is torn down.
         *
         * Walk each rescued root_proto:
         *   1. Capture next_alloc, alloc_fn/alloc_ud before any zero operation.
         *   2. Free all buffers (including nested[] sub-protos) via
         *      uproto_destroy_buffers — zeroes *rp.
         *   3. Free the root_proto struct itself. */
        {
            struct UProto *rp = vm->rescued_protos;
            while (rp != NULL) {
                /* Step 1: capture before any zero operation. */
                struct UProto  *next     = rp->next_alloc;
                UChunkAllocFn  rp_alloc = rp->alloc_fn;
                void           *rp_ud    = rp->alloc_ud;
                if (rp_alloc == NULL) {
                    rp_alloc = vm->alloc_fn;
                    rp_ud    = vm->alloc_ud;
                }
                /* Step 2: free all buffers (nested[] freed recursively inside).
                 * source_name first — uproto_destroy_buffers does not own it
                 * (uchunk_destroy_internal frees it separately; mirror that
                 * here).  Rescued roots from the REPL path have NULL
                 * source_name; tool-driver roots (refactor-3 VM-11) carry
                 * "<expr>" / the script path. */
                if (rp->source_name != NULL) {
                    rp_alloc(rp->source_name, 0, rp_ud);
                    rp->source_name = NULL;
                }
                uproto_destroy_buffers(rp, rp_alloc, rp_ud);
                /* Step 3: free the root_proto struct. */
                rp_alloc(rp, 0, rp_ud);
                rp = next;
            }
            vm->rescued_protos = NULL;
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
    /* Bootstrap GC pause (v0.13.2): the register functions hold fresh
     * cells in C locals across further allocations (closure across
     * intern + slot install).  Host code, no strand — see the rationale
     * banner above populate_realm_globals_impl in realm/urealm_globals.c.
     * Guarded here as well (not just in the populate wrapper) because
     * unit tests call this entry point directly on armed stress builds.
     * Save/restore so a host-held urbi_gc_pause latch survives. */
    uint8_t saved_pause = vm->gc_paused;
    UVMError err;
    vm->gc_paused = 1U;

    /* Propagate UVM_OOM via vm->last_error so callers can detect failure.
     * Both urbi_event_native_register and urbi_tag_native_register (TAGCH-004) now
     * return UVMError so partial-init OOM is surfaced rather than silently
     * leaving NULL protos behind. */
    err = urbi_event_native_register(vm);
    if (err != UVM_OK) {
        vm->last_error = err;
        vm->gc_paused = saved_pause;
        return;
    }
    err = urbi_tag_native_register(vm);
    if (err != UVM_OK) {
        vm->last_error = err;
    }
    vm->gc_paused = saved_pause;
}

/* === urbi_register_event_drain (T57 — spec #3 §9) ===
 *
 * Install a host callback that is invoked at each safepoint (urbi_step entry)
 * for every entry drained from the ISR SPSC ring.  The handler maps event_id
 * to a UEvent* and typically calls urbi_event_emit_async.  Pass NULL to remove
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
/* v0.10.3 (W3): gains void *ud parameter; event_drain_ud stored alongside. */
void
urbi_register_event_drain(UVM *vm, urbi_event_drain_handler h, void *ud)
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
    /* Store ud before handler so that a concurrent reader (v1.x) sees
     * the ud before the handler pointer (handler is the "live" signal). */
    vm->event_drain_ud = ud;
    /* EVENT-007: __ATOMIC_RELEASE store pairs with the __ATOMIC_ACQUIRE
     * load in uevent_ring_drain.  Single-threaded today; the pairing
     * inherits correctness for v1.x URBI_SCHED_PREEMPTIVE. */
    __atomic_store_n(&vm->event_drain_handler, h, __ATOMIC_RELEASE);
}

/* v0.10.3 (W3): UVMError retired; parameter is now int.
 * Returns the legacy name for backward compat with tests that check
 * the string. UVM_OK == URBI_OK == 0; UVM_OOM == URBI_ERR_OOM == -3;
 * UVM_TYPE_ERROR == URBI_ERR_STRAND_FATAL == -2. */
const char *uvm_error_name(int code) {
    if (code == 0)  return "UVM_OK";
    if (code == -3) return "UVM_OOM";
    if (code == -2) return "UVM_TYPE_ERROR";
    return "UVM_UNKNOWN";
}
