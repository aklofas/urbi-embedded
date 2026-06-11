/* SPDX-License-Identifier: BSD-3-Clause */
/* test_tag_self_block — refactor-3 VM-03/B12 self-suspend exit arm.
 *
 * Source: var t = Tag.new(); t: { t.block(); Realm.hit9 = 1 }
 *
 * t.block() from inside the tag's own scope hits urbi_strand_suspend's
 * RUNNING arm: the state byte is stamped SUSPENDED while the strand is
 * mid-dispatch inside OP_CALL's native-dispatch path.  Pre-fix the
 * post-native check only caught USTRAND_IS_WAITING (0x30), not
 * SUSPENDED (0x50), so dispatch fell through to NEXT() and kept running
 * the suspended strand — the suspension evaporates (Realm.hit9 is set
 * immediately, the strand runs to DEAD and is reaped).
 *
 * Post-fix contract (both cases):
 *   1. bounded urbi_step driving parks the strand SUSPENDED with the
 *      right reason sub-code; hit9 is NOT yet set; no abort, no spin
 *      (urbi_step converges to QUIESCENT — a suspended strand does not
 *      count toward strand_runnable_count, matching the READY-arm
 *      convention where sched_strand_unbind_from_ready_queue
 *      decrements);
 *   2. urbi_tag_unblock / urbi_tag_unfreeze resumes the strand AFTER
 *      the blocking call: hit9 == 1, the strand runs to DEAD and is
 *      eager-reaped off realm->strands_head, the VM is quiescent.
 *
 * Resume-value delivery into the result register (v0.10.9-C / W3f) is
 * deferred and NOT asserted here — resume just continues execution. */

#include "utest.h"

#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "sched/ustrand.h"
#include "sched/usched_cooperative.h"
#include "chunk/uchunk.h"
#include "realm/urealm.h"
#include "value/uarena.h"
#include "runtime/umacros.h"
#include "lex/ulex.h"
#include "parse/uast.h"
#include "parse/uparse.h"
#include "emit/uemit.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* ===================================================================
 * Helpers (compile + drive boilerplate mirrors test_cleanup_yield.c)
 * =================================================================== */

/* Compile `src` into a heap-allocated module (heap so a fatal loader
 * strand's undischarged root_proto ref routes through rescued_protos
 * instead of dangling on a stack address — mirrors utest_e2e_helpers). */
static UProto *
compile_heap_chunk(UVM *vm, const char *src)
{
    UProto *module = (UProto *)vm->alloc_fn(NULL, sizeof(UProto), vm->alloc_ud);
    if (module == NULL) return NULL;
    urbi_zero(module, sizeof(*module));
    module->heap_allocated = true;
    module->alloc_fn       = vm->alloc_fn;
    module->alloc_ud       = vm->alloc_ud;

    UArena arena;
    uarena_init(&arena, 4096);

    ULexer lex;
    ulex_init(&lex, src, strlen(src));
    UEmitter e;
    uemit_init(&e, module, &arena, vm, NULL);
    UParser p;
    uparse_init(&p, &lex, &arena);

    bool ok = true;
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) { ok = false; break; }
        if (uemit_statement(&e, node) != EMIT_OK) { ok = false; break; }
        uarena_reset(&arena);
    }
    if (ok && uemit_finish(&e) != EMIT_OK) ok = false;
    uarena_destroy(&arena);

    if (!ok) {
        uchunk_destroy(module, vm);
        return NULL;
    }
    return module;
}

/* Pump urbi_step until it stops reporting RUNNING (or the cap is hit).
 * Returns the last UStepResult. */
static UStepResult
pump_steps(UVM *vm, int max_steps)
{
    UStepResult rc = URBI_STEP_RUNNING;
    for (int i = 0; i < max_steps; i++) {
        rc = urbi_step(vm, 1000, NULL);
        if (rc != URBI_STEP_RUNNING) break;
    }
    return rc;
}

/* Arm a loader strand for `module` and drive urbi_step bounded.
 * *out_loader receives the strand pointer — valid for ADDRESS COMPARISON
 * only; deref it only after find_strand_in_realm confirms it is still
 * linked (post_dispatch eager-reaps DEAD strands). */
static UStepResult
drive_chunk(UVM *vm, UProto *module, UStrand **out_loader, int max_steps)
{
    URealm *realm = urbi_realm_global(vm);

    UStrand *loader = urbi_strand_create_for_module(vm, realm, module);
    if (out_loader != NULL) *out_loader = loader;
    if (loader == NULL) return URBI_STEP_FATAL;

    return pump_steps(vm, max_steps);
}

/* Walk realm->strands_head for a strand by address.  Returns the strand
 * if still linked (safe to deref), NULL if it has been reaped. */
static UStrand *
find_strand_in_realm(UVM *vm, const UStrand *needle)
{
    URealm *realm = urbi_realm_global(vm);
    for (UStrand *p = realm->strands_head; p != NULL; p = p->next_in_realm)
        if (p == needle) return p;
    return NULL;
}

/* Read Realm.hit9 as an int; returns -1 on lookup failure / non-int. */
static int64_t
read_hit9(UVM *vm)
{
    URealm *gr = urbi_realm_global(vm);
    UValue v = {0};
    if (urbi_realm_get_global(vm, gr, "hit9", 4, &v) != URBI_OK) return -1;
    if (v.kind != (uint8_t)UVAL_INT) return -1;
    return v.v.i;
}

/* ===================================================================
 * Case 1: t.block() from inside the tag's own scope parks the strand
 * SUSPENDED (REASON_BLOCK); urbi_tag_unblock resumes it AFTER the
 * t.block() call.
 * =================================================================== */
UTEST(self_block_suspends_then_resumes)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *gr = urbi_realm_global(&vm);
    UASSERT(gr != NULL);
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, gr, "hit9", 4,
                                              urbi_make_int(0)));

    UProto *module = compile_heap_chunk(&vm,
        "var t = Tag.new(); t: { t.block(); Realm.hit9 = 1 }");
    UASSERT(module != NULL);

    UStrand *loader = NULL;
    UStepResult rc = drive_chunk(&vm, module, &loader, 100);

    /* 1. No abort, no fatal, no spin: the strand parks SUSPENDED and the
     * VM converges (a SUSPENDED strand is excluded from liveness — the
     * host that suspended it is responsible for resuming it). */
    UASSERT_EQ((int)URBI_STEP_QUIESCENT, (int)rc);
    UASSERT(vm.fatal_strand == NULL);
    UASSERT_EQ(0U, vm.strand_runnable_count);

    /* hit9 must NOT be set while suspended (pre-fix: evaporation set it). */
    UASSERT_EQ((int64_t)0, read_hit9(&vm));

    /* 2. The strand is still linked on the realm (NOT reaped) and is
     * SUSPENDED with REASON_BLOCK; the tag is recorded in wait_payload. */
    UStrand *s = find_strand_in_realm(&vm, loader);
    UASSERT(s != NULL);
    if (s != NULL) {
        UASSERT_EQ((unsigned)USTRAND_SUSPENDED, (unsigned)USTRAND_GET_STATE(s));
        UASSERT_EQ((unsigned)USTRAND_REASON_BLOCK,
                   (unsigned)USTRAND_GET_REASON(s));
        UASSERT(s->wait_payload.suspend_tag != NULL);

        /* 3. Unblock via the C API and drive again. */
        UASSERT_EQ(URBI_OK,
                   urbi_tag_unblock(&vm, s->wait_payload.suspend_tag));
        UASSERT_EQ(1U, vm.strand_runnable_count);

        rc = pump_steps(&vm, 100);

        /* 4. The strand resumed AFTER the t.block() call and completed:
         * hit9 == 1, strand reaped (DEAD strands are eager-reaped), VM
         * quiescent. */
        UASSERT_EQ((int)URBI_STEP_QUIESCENT, (int)rc);
        UASSERT(vm.fatal_strand == NULL);
        UASSERT_EQ((int64_t)1, read_hit9(&vm));
        UASSERT(find_strand_in_realm(&vm, loader) == NULL);
    }

    uchunk_destroy(module, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Case 2: t.freeze() self-suspend (REASON_FREEZE) — same OP_CALL arm,
 * resumed via urbi_tag_unfreeze.
 * =================================================================== */
UTEST(self_freeze_suspends_then_resumes)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *gr = urbi_realm_global(&vm);
    UASSERT(gr != NULL);
    UASSERT_EQ(URBI_OK, urbi_realm_set_global(&vm, gr, "hit9", 4,
                                              urbi_make_int(0)));

    UProto *module = compile_heap_chunk(&vm,
        "var t = Tag.new(); t: { t.freeze(); Realm.hit9 = 1 }");
    UASSERT(module != NULL);

    UStrand *loader = NULL;
    UStepResult rc = drive_chunk(&vm, module, &loader, 100);

    UASSERT_EQ((int)URBI_STEP_QUIESCENT, (int)rc);
    UASSERT(vm.fatal_strand == NULL);
    UASSERT_EQ(0U, vm.strand_runnable_count);
    UASSERT_EQ((int64_t)0, read_hit9(&vm));

    UStrand *s = find_strand_in_realm(&vm, loader);
    UASSERT(s != NULL);
    if (s != NULL) {
        UASSERT_EQ((unsigned)USTRAND_SUSPENDED, (unsigned)USTRAND_GET_STATE(s));
        UASSERT_EQ((unsigned)USTRAND_REASON_FREEZE,
                   (unsigned)USTRAND_GET_REASON(s));
        UASSERT(s->wait_payload.suspend_tag != NULL);

        UASSERT_EQ(URBI_OK,
                   urbi_tag_unfreeze(&vm, s->wait_payload.suspend_tag));
        UASSERT_EQ(1U, vm.strand_runnable_count);

        rc = pump_steps(&vm, 100);

        UASSERT_EQ((int)URBI_STEP_QUIESCENT, (int)rc);
        UASSERT(vm.fatal_strand == NULL);
        UASSERT_EQ((int64_t)1, read_hit9(&vm));
        UASSERT(find_strand_in_realm(&vm, loader) == NULL);
    }

    uchunk_destroy(module, &vm);
    urbi_vm_destroy(&vm);
}

/* ===================================================================
 * Suite entry point.
 * =================================================================== */

void test_tag_self_block_suite(void);

void
test_tag_self_block_suite(void)
{
    utest_run("tag_self_block: t.block() inside own scope suspends, "
              "unblock resumes after the call",
              self_block_suspends_then_resumes);
    utest_run("tag_self_block: t.freeze() inside own scope suspends, "
              "unfreeze resumes after the call",
              self_freeze_suspends_then_resumes);
}
