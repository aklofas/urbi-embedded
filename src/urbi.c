/* SPDX-License-Identifier: BSD-3-Clause */

#include "urbi/urbi.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "module/umodule.h"
#include "value/uintern.h"
#include "runtime/umacros.h"
#include "object/uic.h"
#include "object/umodule_instance.h"

#if __STDC_HOSTED__
#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>
#endif

#define URBI_VERSION "0.3.0-concurrency"

const char *urbi_version(void) { return URBI_VERSION; }

/* urbi_panic: fatal runtime error.
 * Hosted: writes msg to stderr, then aborts.
 * Freestanding: spins forever (no OS abort). */
URBI_NORETURN void
urbi_panic(const char *msg)
{
#if __STDC_HOSTED__
    fputs(msg, stderr);
    fputc('\n', stderr);
    abort();
#else
    (void)msg;
    /* Freestanding: no abort() available.  Spin to halt execution.
     * Embedded BSPs may override by wrapping or patching this symbol. */
    for (;;) { /* spin */ }
#endif
}

/* urbi_set_isr_check_fn: install an ISR-context predicate.
 * Pass NULL to disable ISR checking (the default after urbi_vm_init). */
void
urbi_set_isr_check_fn(struct UVM *vm, bool (*fn)(void))
{
    if (!vm) return;
    vm->isr_check_fn = fn;
}

/* urbi_set_callback_watchdog_mode: select WARN or ASSERT on slow callback. */
void
urbi_set_callback_watchdog_mode(struct UVM *vm, UWatchdogMode mode)
{
    if (!vm) return;
    vm->callback_watchdog_mode = (uint8_t)mode;
}

/* urbi_call_host_with_watchdog: URBI_DEBUG build implementation.
 * Times fn() using vm->host_time_us; logs or panics if elapsed exceeds
 * vm->callback_warn_us.  Non-debug builds use the macro in urbi.h. */
#ifdef URBI_DEBUG
UValue
urbi_call_host_with_watchdog(struct UVM *vm, struct UStrand *s,
                             UHostFn fn, int argc, UValue *argv)
{
    uint64_t t0      = vm->host_time_us();
    UValue   r       = fn(s, argc, argv);
    uint64_t elapsed = vm->host_time_us() - t0;
    if (elapsed > (uint64_t)vm->callback_warn_us) {
        if (vm->callback_watchdog_mode == URBI_WATCHDOG_ASSERT) {
            urbi_panic("host callback exceeded watchdog threshold");
        } else if (vm->host_log_fn) {
            vm->host_log_fn(vm, URBI_LOG_WARN,
                            "host callback exceeded %u us (took %llu us)",
                            vm->callback_warn_us, (unsigned long long)elapsed);
        }
    }
    return r;
}
#endif /* URBI_DEBUG */

#ifdef URBI_DEBUG

/* --- urbi_get_determinism_checksum implementation --- */

/* FNV-1a 64-bit constants. */
#define FNV1A_SEED   UINT64_C(14695981039346656037)
#define FNV1A_PRIME  UINT64_C(1099511628211)

#define FNV1A_MIX(h, x) \
    do { (h) ^= (uint64_t)(x); (h) *= FNV1A_PRIME; } while (0)

/* Context struct for the namespace walk callback. */
typedef struct {
    uint64_t h;
} UChecksumCtx;

/* unamespace_walk_roots callback: fold each UValue into the running hash.
 * UVAL_INT: hashes the integer value directly.
 * UVAL_BOOL: hashes the integer value (0/1 stored as int64_t).
 * UVAL_STR: hashes the interned pointer address.  Stable within one VM
 *   lifetime (intern table never moves pointers); NOT cross-run-stable
 *   because allocator placement varies between process invocations.
 * UVAL_CLOSURE / UVAL_STRAND / UVAL_VOID / UVAL_NIL: hash only the kind
 *   (heap pointers are not deterministic across runs). */
static void
checksum_walk_cb(struct UVM *vm, UValue *root, void *ctx)
{
    UChecksumCtx *c = (UChecksumCtx *)ctx;
    (void)vm;

    FNV1A_MIX(c->h, root->kind);
    switch (root->kind) {
        case UVAL_INT:
        case UVAL_BOOL:
            FNV1A_MIX(c->h, (uint64_t)root->v.i);
            break;
        case UVAL_FLOAT: {
            /* Mix the float's bit pattern at its actual width.  Reading v.i would
             * include stale upper bytes for f32 (URBI_FLOAT_TYPE==4) when a slot
             * was previously assigned UVAL_INT — non-deterministic. */
#if URBI_FLOAT_TYPE == 8
            uint64_t bits;
            memcpy(&bits, &root->v.f, sizeof(bits));
            FNV1A_MIX(c->h, bits);
#else
            uint32_t bits;
            memcpy(&bits, &root->v.f, sizeof(bits));
            FNV1A_MIX(c->h, (uint64_t)bits);
#endif
            break;
        }
        case UVAL_STR:
            /* Interned pointer: stable within-run identity (see comment above). */
            FNV1A_MIX(c->h, (uintptr_t)root->v.p);
            break;
        default:
            /* NIL, CLOSURE, VOID, STRAND: kind already mixed above. */
            break;
    }
}

/* urbi_get_determinism_checksum: return a stable FNV-1a hash of observable
 * VM state.  Must be called at a QUIESCENT point (no runnable strands).
 * See declaration in urbi.h for full contract. */
uint64_t
urbi_get_determinism_checksum(struct UVM *vm)
{
    URBI_ASSERT_NOT_ISR(vm);
    URBI_INTERNAL_ASSERT(vm != NULL);

    UChecksumCtx ctx;
    struct URealm *r;

    ctx.h = FNV1A_SEED;

    /* 1. All UValue bindings in every live Realm's namespace. */
    for (r = vm->realms_head; r != NULL; r = r->next_in_vm) {
        unamespace_walk_roots(r->bindings, checksum_walk_cb, vm, &ctx);
    }

    /* 2. Watcher pool high-water mark (watcher lifecycle observable state). */
    FNV1A_MIX(ctx.h, (uint64_t)vm->watcher_pool_high_water);

    /* 3. GC total allocated bytes (monotonic allocation counter). */
    FNV1A_MIX(ctx.h, (uint64_t)vm->gc_total_allocated);

    /* 4. Intern table entry count (number of unique strings seen). */
    FNV1A_MIX(ctx.h, (uint64_t)uintern_count(vm));

    /* 5. M4 topology + identity counters (per pre-M4 topology-generation
     *    spec §5 and prototype-chain spec §8.1).  Surfaces non-determinism
     *    in shape-tree mutation ordering, top-level-lookup sequencing, and
     *    UObject identity assignment. */
    FNV1A_MIX(ctx.h, vm->topology_gen);
    FNV1A_MIX(ctx.h, vm->lookup_id);
    FNV1A_MIX(ctx.h, (uint64_t)vm->next_object_id);

    /* 6. M4 T30 — per-IC observable state.  Walk every live UModuleInstance
     *    on the per-VM registry (insertion-order; deterministic in any
     *    well-formed test harness) and, for each UIC site in each
     *    UProtoInstance's IC table, fold in:
     *      - ic->n              (live entry count)
     *      - ic->replace_cursor (round-robin eviction position)
     *      - per entry e in [0, n): topology_gen[e] (cached generation;
     *        ordering of fills observable in the run)
     *
     *    Pointer fields (recv_shapes / slots / uprops) are NOT folded —
     *    they are heap addresses and not stable across process invocations.
     *    The (n, replace_cursor, topology_gen[]) triple is sufficient to
     *    detect ordering divergences across runs because IC fill ordering
     *    is itself driven by topology_gen ticks. */
    {
        const struct UModuleInstance *mi;
        for (mi = vm->module_instances_head; mi != NULL; mi = mi->next_in_vm) {
            const UProtoInstanceArr *arr = mi->proto_instances;
            if (arr == NULL) continue;
            uint16_t i;
            for (i = 0U; i < arr->n; i++) {
                const UProtoInstance *pi = &arr->entries[i];
                if (pi->ic_table == NULL) continue;
                uint16_t ic_count;
                if (pi->proto != NULL) {
                    ic_count = pi->proto->ic_count;
                } else if (i == 0U) {
                    /* Root chunk — read ic_count from UModule. */
                    ic_count = mi->module->ic_count;
                } else {
                    ic_count = 0U;  /* entries[i>0] always have a proto */
                }
                uint16_t k;
                for (k = 0U; k < ic_count; k++) {
                    const UIC *ic = &pi->ic_table[k];
                    FNV1A_MIX(ctx.h, (uint64_t)ic->n);
                    FNV1A_MIX(ctx.h, (uint64_t)ic->replace_cursor);
                    int e;
                    for (e = 0; e < ic->n; e++) {
                        FNV1A_MIX(ctx.h, ic->topology_gen[e]);
                    }
                }
            }
        }
    }

    return ctx.h;
}

#undef FNV1A_MIX
#undef FNV1A_PRIME
#undef FNV1A_SEED

#endif /* URBI_DEBUG */
