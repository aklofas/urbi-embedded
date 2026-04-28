/* SPDX-License-Identifier: BSD-3-Clause */

#include "urbi.h"
#include "uvm.h"
#include "urealm.h"
#include "umodule.h"
#include "uintern.h"
#include "urbi_internal.h"

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
 * Pass NULL to disable ISR checking (the default after uvm_init). */
void
urbi_set_isr_check_fn(struct UVM *vm, bool (*fn)(void))
{
    if (!vm) return;
    vm->isr_check_fn = fn;
}

/* urbi_set_callback_watchdog_mode: select WARN or ASSERT on slow callback. */
void
urbi_set_callback_watchdog_mode(struct UVM *vm, uint8_t mode)
{
    if (!vm) return;
    vm->callback_watchdog_mode = mode;
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
} ChecksumCtx;

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
    ChecksumCtx *c = (ChecksumCtx *)ctx;
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

    ChecksumCtx ctx;
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

    return ctx.h;
}

#undef FNV1A_MIX
#undef FNV1A_PRIME
#undef FNV1A_SEED

#endif /* URBI_DEBUG */
