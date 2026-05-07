/* SPDX-License-Identifier: BSD-3-Clause */
/* Unit tests: do_spawn_body_coroutine happy path + OOM fail-soft (spec #1 §5.3),
 * and spawn / respawn entry-point contract asserts (spec #1 §5.2, T25).
 *
 * T24 cases:
 *   1. watcher_spawn_happy_path:
 *      in_watcher_eval=1 + real watcher → spawn → body_strand non-NULL,
 *      back-pointer set, state READY (USTRAND_READY after urbi_strand_start).
 *   2. watcher_spawn_oom_strand_alloc:
 *      alloc_fn returns NULL for the strand alloc → body_strand stays NULL,
 *      no leak, URBI_LOG_WARN fired exactly once.
 *   3. watcher_spawn_oom_stack_alloc:
 *      alloc_fn allows strand alloc but fails on register-stack alloc →
 *      body_strand stays NULL, strand freed (no realm-list dangler),
 *      URBI_LOG_WARN fired exactly once.
 *
 * T25 cases (URBI_DEBUG only):
 *   4. watcher_spawn_rejects_at_sync:
 *      AT_SYNC mode → spawn_body_coroutine assert fires.
 *   5. watcher_respawn_skips_eval_assert:
 *      respawn_body_coroutine with in_watcher_eval=0 succeeds. */

#include "utest.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "sched/ustrand.h"
#include "module/umodule.h"       /* UClosure, UProto */
#include "runtime/uclosure.h"
#include "runtime/uframe.h"        /* UVM_STACK_CAP */
#include "watcher/uwatcher.h"
#include "urbi/urbi.h"     /* URBI_LOG_WARN */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#ifdef URBI_DEBUG
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* EXPECT_ABORT: assert that expr causes abort (via assert() failure).
 * Uses fork+waitpid: child executes expr; parent verifies abnormal exit.
 * Only meaningful in URBI_DEBUG builds where URBI_INTERNAL_ASSERT is assert(). */
#define EXPECT_ABORT(expr)                                                   \
    do {                                                                     \
        utest_checks++;                                                      \
        pid_t _pid = fork();                                                 \
        if (_pid == 0) {                                                     \
            (expr);                                                          \
            _exit(0); /* should not reach — abort expected */                \
        }                                                                    \
        int _st = 0;                                                         \
        waitpid(_pid, &_st, 0);                                              \
        int _aborted = WIFSIGNALED(_st) ||                                   \
                       (WIFEXITED(_st) && WEXITSTATUS(_st) != 0);           \
        if (!_aborted) {                                                     \
            utest_failures++;                                                \
            printf("  FAIL: %s:%d: " #expr " did not abort\n",              \
                   __FILE__, __LINE__);                                      \
            fflush(stdout);                                                  \
        }                                                                    \
    } while (0)
#endif /* URBI_DEBUG */

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers
 * =================================================================== */

/* Build a minimal UClosure wrapping a stack-local UProto with a single
 * OP_RET instruction.  proto/closure/instr storage is caller-provided. */
static void
make_trivial_closure(UClosure *cl, UProto *proto, uint32_t *instr_buf)
{
    instr_buf[0] = (uint32_t)OP_RET;

    memset(proto, 0, sizeof(*proto));
    proto->instructions = instr_buf;
    proto->instr_count  = 1;
    proto->constants    = NULL;
    proto->const_count  = 0;

    memset(cl, 0, sizeof(*cl));
    cl->proto   = proto;
    cl->nupvals = 0;
}

/* Install a minimal watcher with a real body closure and a realm back-pointer.
 * owning_tag is set to realm->tag so the ambient-attach path is skipped
 * (the happy-path test focuses on the core spawn sequence). */
static UWatcher *
make_body_watcher(struct UVM *vm, struct URealm *realm,
                  UClosure *body_cl)
{
    UWatcher *w = urbi_watcher_install_internal(
        vm, UWATCHER_AT,
        realm->tag,   /* owning_tag == realm->tag → no extra attach */
        NULL,         /* condition */
        body_cl,      /* body */
        NULL,         /* onleave */
        NULL, 0U);
    if (w) {
        w->realm = realm;
    }
    return w;
}

/* Log capture state. */
static int g_log_count_warn;
static int g_log_count_total;

static void
capture_log_fn(struct UVM *vm, int level, const char *fmt, ...)
{
    (void)vm; (void)fmt;
    g_log_count_total++;
    if (level == URBI_LOG_WARN) g_log_count_warn++;
}

/* Failing allocator state: allows `allow` new allocs then fails. */
typedef struct {
    size_t allow;   /* remaining allocs to permit */
} FailAlloc;

static void *
fail_after_n_alloc(void *ptr, size_t nbytes, void *ud)
{
    FailAlloc *fa = (FailAlloc *)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    if (fa->allow == 0) return NULL;   /* fail */
    fa->allow--;
    return malloc(nbytes);
}

/* Null allocator: always fails new allocs, passes frees. */
static void *
null_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ud;
    if (nbytes == 0) { free(ptr); return NULL; }
    return NULL;
}

/* Count the strands on realm->strands_head. */
static int
count_realm_strands(struct URealm *r)
{
    int n = 0;
    UStrand *s = r->strands_head;
    while (s) { n++; s = s->next_in_realm; }
    return n;
}

/* ===================================================================
 * Test cases
 * =================================================================== */

/* 1. watcher_spawn_happy_path
 *
 * Install an AT watcher with a real body closure; call do_spawn_body_coroutine;
 * verify:
 *   - w->body_strand != NULL
 *   - w->body_strand->watcher_body_owner == w (back-pointer)
 *   - w->body_strand->state == USTRAND_READY (urbi_strand_start enqueued it) */
UTEST(watcher_spawn_happy_path)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;

    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_trivial_closure(&body_cl, &proto, instr);

    vm.in_watcher_eval = 1;

    UWatcher *w = make_body_watcher(&vm, r, &body_cl);
    UASSERT(w != NULL);
    UASSERT(w->body_strand == NULL);  /* no body strand before spawn */

    do_spawn_body_coroutine(&vm, w, NULL);

    /* body_strand must be set. */
    UASSERT(w->body_strand != NULL);
    /* Back-pointer must be correct. */
    UASSERT(w->body_strand->watcher_body_owner == w);
    /* urbi_strand_start transitions DORMANT → READY (enqueues on run-queue). */
    UASSERT_EQ((unsigned)w->body_strand->state, (unsigned)USTRAND_READY);

    vm.in_watcher_eval = 0;

    /* Clean up — unregister watcher (releases from pool) then destroy realm
     * (walks strands_head and frees all realm-managed strands). */
    urbi_watcher_unregister_internal(&vm, w);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* 2. watcher_spawn_oom_strand_alloc
 *
 * Install the VM with a null allocator (new allocs always fail).  The strand
 * alloc inside urbi_strand_create fails immediately.
 * Verify:
 *   - w->body_strand stays NULL
 *   - URBI_LOG_WARN fired exactly once
 *   - No strand leaked into realm->strands_head */
UTEST(watcher_spawn_oom_strand_alloc)
{
    /* Use the normal allocator for setup, switch to null before spawn. */
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;

    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_trivial_closure(&body_cl, &proto, instr);

    UWatcher *w = make_body_watcher(&vm, r, &body_cl);
    UASSERT(w != NULL);

    /* Count strands before spawn. */
    int strands_before = count_realm_strands(r);

    /* Reset log counters and install log capture. */
    g_log_count_warn  = 0;
    g_log_count_total = 0;
    vm.host_log_fn = capture_log_fn;

    /* Switch allocator to null so the strand allocation inside
     * urbi_strand_create returns NULL. */
    UVMAllocFn saved_alloc = vm.alloc_fn;
    void       *saved_ud   = vm.alloc_ud;
    vm.alloc_fn = null_alloc;
    vm.alloc_ud = NULL;

    vm.in_watcher_eval = 1;
    do_spawn_body_coroutine(&vm, w, NULL);
    vm.in_watcher_eval = 0;

    /* Restore allocator before cleanup assertions. */
    vm.alloc_fn = saved_alloc;
    vm.alloc_ud = saved_ud;

    /* body_strand must still be NULL. */
    UASSERT(w->body_strand == NULL);

    /* Exactly one URBI_LOG_WARN must have been emitted. */
    UASSERT_EQ(g_log_count_warn, 1);

    /* No strand leaked. */
    UASSERT_EQ(count_realm_strands(r), strands_before);

    urbi_watcher_unregister_internal(&vm, w);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* 3. watcher_spawn_oom_stack_alloc
 *
 * Allow the strand alloc + cleanup-stack alloc (urbi_strand_create internally
 * calls ustrand_init which calls strand_cleanup_stack_init) to succeed, but
 * fail the register-stack alloc inside urbi_strand_arm_from_closure.
 *
 * The implementation must:
 *   - Call urbi_strand_destroy (removing strand from realm list + freeing it).
 *   - Emit URBI_LOG_WARN exactly once.
 *   - Leave w->body_strand == NULL. */
UTEST(watcher_spawn_oom_stack_alloc)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;

    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_trivial_closure(&body_cl, &proto, instr);

    UWatcher *w = make_body_watcher(&vm, r, &body_cl);
    UASSERT(w != NULL);

    /* Calibrate: count allocs consumed by urbi_vm_init + urbi_realm_create +
     * urbi_watcher_install_internal.  We'll allow that many allocs plus
     * enough for urbi_strand_create (UStrand + cleanup-stack) but deny
     * the register-stack alloc.
     *
     * Strategy: count allocs up to this point using a counting shim,
     * then switch to a fail-after-N allocator that permits exactly the
     * allocs needed for urbi_strand_create but fails on the next one
     * (register-stack inside urbi_strand_arm_from_closure).
     *
     * Simpler approach: use a FailAlloc that allows exactly 2 allocs
     * (UStrand + cleanup-stack array) then fails.  The exact count may
     * vary if ustrand_init's cleanup stack requires more than one alloc.
     * We calibrate by counting realm strands: after a successful create +
     * failed arm, the strand must be gone from the realm list. */

    int strands_before = count_realm_strands(r);

    g_log_count_warn  = 0;
    g_log_count_total = 0;
    vm.host_log_fn = capture_log_fn;

    /* Allow exactly 2 new allocations: one for UStrand, one for the
     * cleanup-stack array (URBI_CLEANUP_MAX entries).  The third alloc
     * (register-stack in urbi_strand_arm_from_closure: UVM_STACK_CAP * 16B)
     * will fail.  If ustrand_init requires fewer allocs, increase the limit
     * in future; 2 is the minimum for a successful urbi_strand_create. */
    FailAlloc fa;
    fa.allow = 2;

    UVMAllocFn saved_alloc = vm.alloc_fn;
    void       *saved_ud   = vm.alloc_ud;
    vm.alloc_fn = fail_after_n_alloc;
    vm.alloc_ud = &fa;

    vm.in_watcher_eval = 1;
    do_spawn_body_coroutine(&vm, w, NULL);
    vm.in_watcher_eval = 0;

    vm.alloc_fn = saved_alloc;
    vm.alloc_ud = saved_ud;

    /* body_strand must still be NULL. */
    UASSERT(w->body_strand == NULL);

    /* Exactly one URBI_LOG_WARN must have been emitted. */
    UASSERT_EQ(g_log_count_warn, 1);

    /* The partially-constructed strand must have been destroyed and removed
     * from the realm list — strand count must not grow. */
    UASSERT(count_realm_strands(r) <= strands_before);

    urbi_watcher_unregister_internal(&vm, w);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * T25 cases (spec #1 §5.2 entry-point contract)
 * =================================================================== */

#ifdef URBI_DEBUG

/* 4. watcher_spawn_rejects_at_sync
 *
 * AT_SYNC bodies run inline on the scratch frame and must never go through
 * spawn_body_coroutine.  In URBI_DEBUG builds the mode assert fires. */
UTEST(watcher_spawn_rejects_at_sync)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;

    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_trivial_closure(&body_cl, &proto, instr);

    UWatcher *w = make_body_watcher(&vm, r, &body_cl);
    UASSERT(w != NULL);

    /* Override mode to AT_SYNC after install. */
    w->mode = UWATCHER_AT_SYNC;
    vm.in_watcher_eval = 1;

    /* AT_SYNC mode must trigger the mode assert inside spawn_body_coroutine. */
    EXPECT_ABORT(spawn_body_coroutine(&vm, w));

    vm.in_watcher_eval = 0;

    urbi_watcher_unregister_internal(&vm, w);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* 5. watcher_respawn_skips_eval_assert
 *
 * respawn_body_coroutine is the completion-path entry: it must succeed even
 * when in_watcher_eval == 0 (the eval guard applies only to spawn_body_coroutine).
 * Verify body_strand is set after a successful respawn call. */
UTEST(watcher_respawn_skips_eval_assert)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;

    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_trivial_closure(&body_cl, &proto, instr);

    UWatcher *w = make_body_watcher(&vm, r, &body_cl);
    UASSERT(w != NULL);

    /* in_watcher_eval == 0: this is the completion path. */
    UASSERT(vm.in_watcher_eval == 0);

    respawn_body_coroutine(&vm, w);

    /* Spawn must have succeeded — body_strand non-NULL. */
    UASSERT(w->body_strand != NULL);

    urbi_watcher_unregister_internal(&vm, w);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

#endif /* URBI_DEBUG */

/* ===================================================================
 * T26 cases (spec #1 §3.2 + §5.3 step 1: exhaust-policy gating)
 * =================================================================== */

/* 6. watcher_spawn_queues_pending_refire_when_body_alive
 *
 * With body_strand already set (body running) and URBI_EXHAUST_QUEUE policy,
 * do_spawn_body_coroutine must:
 *   - Leave body_strand pointing at the sentinel (no new strand allocated).
 *   - Set URBI_WATCHER_PENDING_REFIRE in flags. */
UTEST(watcher_spawn_queues_pending_refire_when_body_alive)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;

    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_trivial_closure(&body_cl, &proto, instr);

    UWatcher *w = make_body_watcher(&vm, r, &body_cl);
    UASSERT(w != NULL);

    /* Pretend the body is already running. */
    UStrand sentinel;
    memset(&sentinel, 0, sizeof(sentinel));
    w->body_strand    = &sentinel;
    w->exhaust_policy = URBI_EXHAUST_QUEUE;

    vm.in_watcher_eval = 1;
    do_spawn_body_coroutine(&vm, w, NULL);
    vm.in_watcher_eval = 0;

    /* body_strand must be unchanged — sentinel pointer still there. */
    UASSERT(w->body_strand == &sentinel);
    /* PENDING_REFIRE bit must be set. */
    UASSERT((w->flags & URBI_WATCHER_PENDING_REFIRE) != 0);

    /* Reset body_strand so teardown doesn't walk the sentinel as a live strand. */
    w->body_strand = NULL;

    urbi_watcher_unregister_internal(&vm, w);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* 7. watcher_spawn_drops_silently_under_exhaust_drop
 *
 * With body_strand already set and URBI_EXHAUST_DROP policy,
 * do_spawn_body_coroutine must:
 *   - Leave body_strand pointing at the sentinel.
 *   - NOT set URBI_WATCHER_PENDING_REFIRE (silent drop). */
UTEST(watcher_spawn_drops_silently_under_exhaust_drop)
{
    UVM vm;
    uint32_t instr[1];
    UProto   proto;
    UClosure body_cl;

    urbi_vm_init(&vm, NULL, NULL);

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    make_trivial_closure(&body_cl, &proto, instr);

    UWatcher *w = make_body_watcher(&vm, r, &body_cl);
    UASSERT(w != NULL);

    UStrand sentinel;
    memset(&sentinel, 0, sizeof(sentinel));
    w->body_strand    = &sentinel;
    w->exhaust_policy = URBI_EXHAUST_DROP;

    vm.in_watcher_eval = 1;
    do_spawn_body_coroutine(&vm, w, NULL);
    vm.in_watcher_eval = 0;

    /* body_strand must still point at the sentinel. */
    UASSERT(w->body_strand == &sentinel);
    /* PENDING_REFIRE must NOT be set (silent drop). */
    UASSERT((w->flags & URBI_WATCHER_PENDING_REFIRE) == 0);

    w->body_strand = NULL;

    urbi_watcher_unregister_internal(&vm, w);
    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry
 * =================================================================== */

void
test_watcher_spawn_suite(void)
{
    printf("test_watcher_spawn\n");
    utest_run("watcher_spawn_happy_path",      watcher_spawn_happy_path);
    utest_run("watcher_spawn_oom_strand_alloc", watcher_spawn_oom_strand_alloc);
    utest_run("watcher_spawn_oom_stack_alloc",  watcher_spawn_oom_stack_alloc);
    utest_run("watcher_spawn_queues_pending_refire_when_body_alive",
              watcher_spawn_queues_pending_refire_when_body_alive);
    utest_run("watcher_spawn_drops_silently_under_exhaust_drop",
              watcher_spawn_drops_silently_under_exhaust_drop);
#ifdef URBI_DEBUG
    utest_run("watcher_spawn_rejects_at_sync",     watcher_spawn_rejects_at_sync);
    utest_run("watcher_respawn_skips_eval_assert",  watcher_respawn_skips_eval_assert);
#endif
}
