/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: OP_GETSLOT trace probe for watcher install (T36, spec #2 §7.3).
 *
 * T36 cases:
 *   1. trace_records_slot_reads_during_install:
 *      Set in_watcher_install=1, run OP_GETSLOT against a real UObject.
 *      Verify trace_read_set[0] == (UCell *)obj and trace_read_set_count == 1.
 *   2. trace_deduplicates_same_receiver:
 *      Run OP_GETSLOT twice with the same receiver.
 *      trace_read_set_count stays at 1 (dedupe by linear scan).
 *   3. trace_overflow_sets_flag_and_caps:
 *      Run OP_GETSLOT URBI_WATCHER_READSET_MAX+2 times with distinct receivers.
 *      trace_read_set_count must clamp to URBI_WATCHER_READSET_MAX; trace_overflow=1.
 *   4. trace_disabled_when_flag_clear:
 *      in_watcher_install=0 (default): OP_GETSLOT does NOT touch trace_read_set_count.
 *   5. install_arms_trace_fields:
 *      install_watcher_runtime (non-recursive path) sets in_watcher_install=1,
 *      trace_overflow=0, trace_read_set_count=0 before returning the stub OK. */

#include "utest.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "module/umodule.h"                        /* UModule, UProto, uinstr_enc_abc */
#include "value/uintern.h"                        /* ustr_intern */
#include "object/uobject.h"                 /* UObject, urbi_object_alloc,
                                               urbi_object_set_local_slot */
#include "object/uic.h"                     /* UIC */
#include "object/umodule_instance.h"         /* urbi_module_instance_create,
                                               UModuleInstance, UProtoInstance */
#include "sched/usched_cooperative.h"       /* sched_init */
#include "watcher/uwatcher.h"               /* UWATCHER_AT */
#include "watcher/uwatcher_install.h"       /* install_watcher_runtime */
#include "urbi/urbi.h"                      /* URBI_LOG_WARN */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Bytecode / strand setup helpers
 * =================================================================== */

/* Allocate a UObject with one slot named "x" = integer 42.
 * Caller owns the GC reference (pinned via urbi_pin before use). */
static UObject *
make_object_with_x_slot(UVM *vm)
{
    UObject *obj = urbi_object_alloc(vm, URBI_ATOM_OBJECT);
    if (obj == NULL) return NULL;
    USymbol *x = (USymbol *)ustr_intern(vm, "x", 1);
    UValue v;
    v.kind  = UVAL_INT;
    v.v.i   = 42;
    (void)urbi_object_set_local_slot(vm, obj, x, v);
    return obj;
}

/* Set up a UModule with ic_count=1 for the root proto ("x" IC site 0).
 * Caller must umodule_destroy(&m) when done.
 * Returns 1 on success, 0 on failure. */
static int
make_module_with_one_ic_site(UVM *vm, UModule *m, uint32_t *instrs_out,
                              size_t max_instrs)
{
    (void)max_instrs;
    memset(m, 0, sizeof(*m));

    /* Task 11: all chunk-top data lives on root_proto. Allocate a UProto. */
    UProto *rp = (UProto *)calloc(1, sizeof(UProto));
    if (rp == NULL) return 0;
    rp->alloc_fn = vm->alloc_fn;
    rp->alloc_ud = vm->alloc_ud;
    m->root_proto = rp;

    /* Root proto ic_count = 1; ic_names points to "x". */
    rp->ic_count = 1;
    rp->ic_names = (USymbol **)malloc(sizeof(USymbol *));
    if (rp->ic_names == NULL) { free(rp); m->root_proto = NULL; return 0; }
    rp->ic_names[0] = (USymbol *)ustr_intern(vm, "x", 1);

    /* Bytecode: OP_GETSLOT R[1] = R[0].slot[0]; OP_RET R[0].
     *   A=1 (dst), B=0 (recv_reg), C=0 (ic_index). */
    instrs_out[0] = uinstr_enc_abc(OP_GETSLOT, 1U, 0U, 0U);
    instrs_out[1] = uinstr_enc_abc(OP_RET,     0U, 0U, 0U);

    rp->instructions = instrs_out;
    rp->instr_count  = 2;
    return 1;
}

/* Init a minimal strand pointing at `instrs` with reg_stack as its registers.
 * R[0] is set to a UVAL_OBJECT pointing at `obj`.
 * s->module_instance is wired to `mi`. */
static void
strand_setup_for_getslot(UStrand *s, UVM *vm,
                         const uint32_t *instrs,
                         UValue *reg_stack,
                         UObject *obj,
                         UModuleInstance *mi)
{
    /* Zero-init via volatile to silence compilers complaining about the
     * partially-initialised struct (UStrand has many fields). */
    volatile unsigned char *p = (volatile unsigned char *)s;
    size_t n = sizeof(*s);
    size_t i;
    for (i = 0; i < n; i++) p[i] = 0;

    s->vm              = vm;
    s->state           = USTRAND_STATE_RUNNING;
    s->stack           = reg_stack;
    s->R               = reg_stack;
    s->pc              = instrs;
    s->pc_base         = instrs;
    s->module          = NULL;
    s->module_instance = mi;
    s->frame_count     = 0;
    s->open_upvals     = NULL;
    s->out_slot        = NULL;

    /* R[0] = the object receiver. */
    reg_stack[0].kind  = (uint8_t)UVAL_OBJECT;
    reg_stack[0].v.p   = obj;
}

/* Run a single OP_GETSLOT (followed by OP_RET) against `obj` on `vm`.
 * Requires vm->topology_gen to be initialised (non-zero sentinel — see urbi_vm_init).
 * Returns the number of opcodes consumed by dispatch_loop_until_yield. */
static uint64_t
run_one_getslot(UVM *vm, UObject *obj)
{
    static uint32_t instrs[2];
    static UValue   reg_stack[8];
    UModule         m;
    UStrand         s;

    memset(reg_stack, 0, sizeof(reg_stack));

    if (!make_module_with_one_ic_site(vm, &m, instrs, 2)) return 0;

    UModuleInstance *mi = urbi_get_or_create_module_instance(vm, &m);
    if (mi == NULL) { free(m.root_proto->ic_names); free(m.root_proto); m.root_proto = NULL; return 0; }

    /* Wire the IC name so the slow path can resolve it on first miss. */
    UProtoInstance *pi = &mi->proto_instances->entries[0];
    if (pi->ic_table != NULL) {
        pi->ic_table[0].name = (USymbol *)ustr_intern(vm, "x", 1);
    }

    strand_setup_for_getslot(&s, vm, instrs, reg_stack, obj, mi);

    uint64_t consumed = dispatch_loop_until_yield(&s, 10000U);

    /* Module IC names are heap-allocated in this helper; umodule_destroy
     * would free instructions (stack here), so only free ic_names + root_proto manually. */
    free(m.root_proto->ic_names);
    m.root_proto->ic_names = NULL;
    free(m.root_proto);
    m.root_proto = NULL;

    return consumed;
}

/* ===================================================================
 * Test cases
 * =================================================================== */

/* 1. trace_records_slot_reads_during_install
 *
 * With in_watcher_install=1, running OP_GETSLOT must append the receiver's
 * UCell* to trace_read_set and increment trace_read_set_count to 1. */
UTEST(trace_records_slot_reads_during_install)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UObject *obj = make_object_with_x_slot(&vm);
    UASSERT(obj != NULL);

    vm.in_watcher_install   = 1;
    vm.trace_overflow        = 0;
    vm.trace_read_set_count  = 0;

    uint64_t consumed = run_one_getslot(&vm, obj);
    UASSERT(consumed >= 1);

    UASSERT_EQ(1, (int)vm.trace_read_set_count);
    UASSERT(vm.trace_read_set[0] == (UCell *)obj);
    UASSERT_EQ(0, (int)vm.trace_overflow);

    vm.in_watcher_install = 0;
    urbi_vm_destroy(&vm);
}

/* 2. trace_deduplicates_same_receiver
 *
 * Running OP_GETSLOT twice against the same object while in_watcher_install=1
 * must leave trace_read_set_count at 1 (dedupe by linear scan). */
UTEST(trace_deduplicates_same_receiver)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UObject *obj = make_object_with_x_slot(&vm);
    UASSERT(obj != NULL);

    vm.in_watcher_install   = 1;
    vm.trace_overflow        = 0;
    vm.trace_read_set_count  = 0;

    /* First read: records the cell. */
    run_one_getslot(&vm, obj);
    UASSERT_EQ(1, (int)vm.trace_read_set_count);

    /* Second read: same receiver, must not add a second entry. */
    run_one_getslot(&vm, obj);
    UASSERT_EQ(1, (int)vm.trace_read_set_count);
    UASSERT_EQ(0, (int)vm.trace_overflow);

    vm.in_watcher_install = 0;
    urbi_vm_destroy(&vm);
}

/* 3. trace_overflow_sets_flag_and_caps
 *
 * Running OP_GETSLOT against URBI_WATCHER_READSET_MAX+2 distinct objects
 * must set trace_overflow=1 and leave trace_read_set_count exactly at
 * URBI_WATCHER_READSET_MAX (no writes beyond the array bound). */
UTEST(trace_overflow_sets_flag_and_caps)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    /* Allocate MAX+2 distinct objects. */
    size_t extra = (size_t)URBI_WATCHER_READSET_MAX + 2U;
    UObject **objs = (UObject **)malloc(extra * sizeof(UObject *));
    UASSERT(objs != NULL);
    size_t i;
    for (i = 0; i < extra; i++) {
        objs[i] = make_object_with_x_slot(&vm);
        UASSERT(objs[i] != NULL);
    }

    vm.in_watcher_install   = 1;
    vm.trace_overflow        = 0;
    vm.trace_read_set_count  = 0;

    for (i = 0; i < extra; i++) {
        run_one_getslot(&vm, objs[i]);
    }

    UASSERT_EQ((int)URBI_WATCHER_READSET_MAX, (int)vm.trace_read_set_count);
    UASSERT_EQ(1, (int)vm.trace_overflow);

    vm.in_watcher_install = 0;
    free(objs);
    urbi_vm_destroy(&vm);
}

/* 4. trace_disabled_when_flag_clear
 *
 * With in_watcher_install=0 (default), OP_GETSLOT must NOT touch
 * trace_read_set_count — the UNLIKELY branch is not taken. */
UTEST(trace_disabled_when_flag_clear)
{
    UVM vm;
    urbi_vm_init(&vm, NULL, NULL);
    sched_init(&vm, NULL);

    UObject *obj = make_object_with_x_slot(&vm);
    UASSERT(obj != NULL);

    /* Default: in_watcher_install == 0. */
    UASSERT_EQ(0, (int)vm.in_watcher_install);
    vm.trace_read_set_count = 0;

    run_one_getslot(&vm, obj);

    UASSERT_EQ(0, (int)vm.trace_read_set_count);

    urbi_vm_destroy(&vm);
}

/* 5. install_arms_and_resets_trace_fields
 *
 * install_watcher_runtime (non-recursive path, no-throw hook) must:
 *   - Phase 2: set in_watcher_install=1, clear trace_overflow and
 *     trace_read_set_count.
 *   - Phase 4: clear in_watcher_install=0 after running the cond stub.
 *   - Return URBI_INSTALL_OK when the cond hook does not throw and
 *     no overflow occurs. */
UTEST(install_arms_and_resets_trace_fields)
{
    UVM vm;
    UStrand s;

    urbi_vm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);

    /* Pre-condition: dirty values to verify the phase-2 reset. */
    vm.trace_overflow       = 1;
    vm.trace_read_set_count = 7;

    UWatcherInstallResult r = install_watcher_runtime(
        &vm, &s, UWATCHER_AT, NULL, NULL, NULL, NULL);

    /* Stub (no hook) returns OK; in_watcher_install must be 0 after phase 4. */
    UASSERT_EQ((int)URBI_INSTALL_OK, (int)r);
    UASSERT_EQ(0, (int)vm.in_watcher_install);
    UASSERT_EQ(0, (int)vm.trace_overflow);

    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T37 test helpers
 * =================================================================== */

/* Hook that simulates overflow: fills trace_read_set_count to MAX then
 * sets trace_overflow=1 (replicating what URBI_WATCHER_READSET_MAX+1
 * OP_GETSLOT hits would do). */
static void
hook_force_overflow(struct UVM *vm, struct UClosure *cond,
                    UValue *out_result, int *out_threw)
{
    UValue nil = {0};
    (void)cond;
    /* Simulate MAX reads + one overflow hit. */
    vm->trace_read_set_count = (uint16_t)URBI_WATCHER_READSET_MAX;
    vm->trace_overflow       = 1;
    *out_result = nil;
    *out_threw  = 0;
}

/* Hook that simulates a cond-throw. */
static void
hook_force_throw(struct UVM *vm, struct UClosure *cond,
                 UValue *out_result, int *out_threw)
{
    UValue nil = {0};
    (void)vm;
    (void)cond;
    *out_result = nil;
    *out_threw  = 1;
}

/* Log-capture state for T37 warn tests. */
static int g_t37_warn_count;
static char g_t37_last_msg[256];

static void
capture_log_t37(struct UVM *vm, int level, const char *fmt, ...)
{
    (void)vm;
    if (level == URBI_LOG_WARN) {
        g_t37_warn_count++;
        strncpy(g_t37_last_msg, fmt, sizeof(g_t37_last_msg) - 1);
        g_t37_last_msg[sizeof(g_t37_last_msg) - 1] = '\0';
    }
}

/* ===================================================================
 * T37 test cases
 * =================================================================== */

/* 6. install_returns_readset_over_when_overflow
 *
 * When the cond hook forces trace_overflow=1, install_watcher_runtime must:
 *   - Return URBI_INSTALL_READSET_OVER.
 *   - Reset in_watcher_install to 0.
 *   - Clear trace_overflow.
 *   - Fire a URBI_LOG_WARN containing "read-set exceeds". */
UTEST(install_returns_readset_over_when_overflow)
{
    UVM vm;
    UStrand s;

    urbi_vm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);

    g_t37_warn_count = 0;
    vm.host_log_fn            = capture_log_t37;
    vm.test_install_cond_hook = hook_force_overflow;

    UWatcherInstallResult r = install_watcher_runtime(
        &vm, &s, UWATCHER_AT, NULL, NULL, NULL, NULL);

    UASSERT_EQ((int)URBI_INSTALL_READSET_OVER, (int)r);
    UASSERT_EQ(0, (int)vm.in_watcher_install);
    UASSERT_EQ(0, (int)vm.trace_overflow);   /* cleared by phase 4 */
    UASSERT_EQ(1, g_t37_warn_count);
    UASSERT(strstr(g_t37_last_msg, "read-set exceeds") != NULL);

    vm.test_install_cond_hook = NULL;
    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* 7. install_returns_trace_fault_on_cond_throw
 *
 * When the cond hook sets *out_threw=1, install_watcher_runtime must:
 *   - Return URBI_INSTALL_TRACE_FAULT.
 *   - Reset in_watcher_install to 0.
 *   - Fire a URBI_LOG_WARN containing "condition threw". */
UTEST(install_returns_trace_fault_on_cond_throw)
{
    UVM vm;
    UStrand s;

    urbi_vm_init(&vm, NULL, NULL);
    ustrand_init(&s, &vm);

    g_t37_warn_count = 0;
    vm.host_log_fn            = capture_log_t37;
    vm.test_install_cond_hook = hook_force_throw;

    UWatcherInstallResult r = install_watcher_runtime(
        &vm, &s, UWATCHER_AT, NULL, NULL, NULL, NULL);

    UASSERT_EQ((int)URBI_INSTALL_TRACE_FAULT, (int)r);
    UASSERT_EQ(0, (int)vm.in_watcher_install);
    UASSERT_EQ(1, g_t37_warn_count);
    UASSERT(strstr(g_t37_last_msg, "condition threw") != NULL);

    vm.test_install_cond_hook = NULL;
    ustrand_destroy(&s, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_install_trace_suite(void)
{
    printf("test_install_trace\n");
    utest_run("trace_records_slot_reads_during_install",
              trace_records_slot_reads_during_install);
    utest_run("trace_deduplicates_same_receiver",
              trace_deduplicates_same_receiver);
    utest_run("trace_overflow_sets_flag_and_caps",
              trace_overflow_sets_flag_and_caps);
    utest_run("trace_disabled_when_flag_clear",
              trace_disabled_when_flag_clear);
    utest_run("install_arms_and_resets_trace_fields",
              install_arms_and_resets_trace_fields);
    utest_run("install_returns_readset_over_when_overflow",
              install_returns_readset_over_when_overflow);
    utest_run("install_returns_trace_fault_on_cond_throw",
              install_returns_trace_fault_on_cond_throw);
}
