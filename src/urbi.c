/* SPDX-License-Identifier: BSD-3-Clause */

#include "urbi/urbi.h"
#include "urbi/version.h"
#include "vm/uvm.h"
#include "realm/urealm.h"
#include "chunk/uchunk.h"
#include "value/uintern.h"
#include "value/uarena.h"
#include "runtime/umacros.h"
#include "object/uic.h"
#include "object/uchunk_instance.h"
#if !defined(URBI_BYTECODE_ONLY)
#  include "lex/ulex.h"
#  include "parse/uparse.h"
#  include "parse/uast.h"
#  include "emit/uemit.h"
#endif
#include <stdint.h>

#if __STDC_HOSTED__
#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>
#endif

/* URBI_VERSION: source-of-truth string returned by urbi_version().
 *
 * API-011: stale at "0.3.0-concurrency" since M3 (2026-04-28), unchanged
 * through v0.4.0/v0.5.0/v0.5.1/v0.5.2/v0.5.3/v0.5.4/v0.5.5/v0.5.6.  The
 * release ritual (CHANGELOG cadence in WORKFLOW.md §8) updates this
 * literal before every annotated tag; the regression test in
 * tests/unit/test_public_api.c::urbi_version_matches_release_tag pins
 * the expected value so a forgotten bump surfaces as a test failure. */
#define URBI_VERSION "0.5.7-fixes"

const char *urbi_version(void) { return URBI_VERSION; }

void urbi_api_version(int *out_major, int *out_minor, int *out_patch) {
    if (out_major) *out_major = URBI_API_VERSION_MAJOR;
    if (out_minor) *out_minor = URBI_API_VERSION_MINOR;
    if (out_patch) *out_patch = URBI_API_VERSION_PATCH;
}

/* urbi_panic: fatal runtime error.
 * Hosted: writes msg to stderr, then aborts.
 * Freestanding: spins forever (no OS abort).
 *
 * API-001: msg may be NULL (defensive); substituted with "<no diagnostic>"
 * before fputs.  fputs(NULL, stderr) is undefined behavior on hosted libcs;
 * the guard makes urbi_panic safe to call from any error path that may not
 * have a message to attach. */
URBI_NORETURN void
urbi_panic(const char *msg)
{
#if __STDC_HOSTED__
    if (!msg) msg = "<no diagnostic>";
    fputs(msg, stderr);
    fputc('\n', stderr);
    abort();
#else
    /* Freestanding: no fputs/abort.  Mark msg consumed (cppcheck would
     * otherwise flag it as never-read) and spin to halt execution.
     * Embedded BSPs may override by wrapping or patching this symbol. */
    (void)msg;
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

/* urbi_set_watcher_body_done_fn: install the watcher-body-completion hook.
 * Pass NULL to uninstall (the default after urbi_vm_init).  NULL vm is a
 * no-op; the cast accepts the public typedef and stores it through the
 * inline-typed slot on UVM (shape-identical).  T33 / spec §7. */
void
urbi_set_watcher_body_done_fn(struct UVM *vm, urbi_watcher_body_done_fn fn)
{
    if (!vm) return;
    vm->watcher_body_done_fn = fn;
}

#if !defined(URBI_BYTECODE_ONLY)
/* urbi_compile_source: compile source → serialized v1.5 bytecode.  See
 * urbi.h for the full contract.  The pipeline is the same one tools/urbi.c
 * uses for in-process REPL/-e/-f compile; this entry point exposes it for
 * the build-time stdlib bake (tools/urbi-compile-stdlib) and any embedder
 * that wants to pre-compile a module.
 *
 * URBI_BYTECODE_ONLY=1 strips this entire function (M7 Wave 1 T17): the
 * header gates the declaration, so the symbol is intentionally absent from
 * freestanding liburbi.a; callers get a compile error at the call site. */
int
urbi_compile_source(struct UVM *vm,
                    const char *src, size_t src_len,
                    const char *src_name,
                    unsigned char **out_buf, size_t *out_len,
                    char *err_buf, size_t err_cap)
{
#if __STDC_HOSTED__
    if (vm == NULL || src == NULL || out_buf == NULL || out_len == NULL) {
        if (err_buf && err_cap) {
            snprintf(err_buf, err_cap, "urbi_compile_source: NULL argument");
        }
        return URBI_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0;

    const char *name = src_name ? src_name : "<source>";

    ULexer lex;
    ulex_init(&lex, src, src_len);

    UArena arena;
    uarena_init(&arena, 4096);

    UModule module;
    urbi_zero(&module, sizeof module);

    UEmitter e;
    uemit_init(&e, &module, &arena, vm, name);

    UParser p;
    uparse_init(&p, &lex, &arena);

    bool had_error = false;
    UAstNode *node;
    while ((node = uparse_next_statement(&p)) != NULL) {
        if (node->kind == AST_ERROR) {
            const char *msg = node->u.err.message ? node->u.err.message
                                                  : "parse error";
            if (err_buf && err_cap) {
                snprintf(err_buf, err_cap, "%s:%d:%d: %s",
                         name, node->line, node->col, msg);
            }
            had_error = true;
            break;
        }
        (void)uemit_statement(&e, node);
        uarena_reset(&arena);
    }

    if (had_error) {
        uchunk_destroy(&module, vm);
        uarena_destroy(&arena);
        return URBI_ERR_INVALID_ARG;
    }

    if (uemit_finish(&e) != EMIT_OK) {
        if (err_buf && err_cap) {
            snprintf(err_buf, err_cap, "%s: emit error: %s",
                     name, uemit_error_name(e.error));
        }
        uchunk_destroy(&module, vm);
        uarena_destroy(&arena);
        return URBI_ERR_INVALID_ARG;
    }

    /* First pass: query required size. */
    ptrdiff_t need = umodule_serialize(&module, NULL, 0);
    if (need < 0) {
        if (err_buf && err_cap) {
            snprintf(err_buf, err_cap, "%s: serialize size-query failed",
                     name);
        }
        uchunk_destroy(&module, vm);
        uarena_destroy(&arena);
        return URBI_ERR_INVALID_ARG;
    }
    unsigned char *buf = malloc((size_t)need);
    if (buf == NULL) {
        if (err_buf && err_cap) {
            snprintf(err_buf, err_cap, "%s: out of memory", name);
        }
        uchunk_destroy(&module, vm);
        uarena_destroy(&arena);
        return URBI_ERR_OOM;
    }
    ptrdiff_t wrote = umodule_serialize(&module, buf, (size_t)need);
    if (wrote != need) {
        if (err_buf && err_cap) {
            snprintf(err_buf, err_cap, "%s: serialize wrote %ld, expected %ld",
                     name, (long)wrote, (long)need);
        }
        free(buf);
        uchunk_destroy(&module, vm);
        uarena_destroy(&arena);
        return URBI_ERR_INVALID_ARG;
    }

    *out_buf = buf;
    *out_len = (size_t)need;
    uchunk_destroy(&module, vm);
    uarena_destroy(&arena);
    return URBI_OK;
#else
    /* Freestanding: compile-from-source is not part of the embedded surface.
     * Pre-compiled bytecode comes in via urbi_load_module instead. */
    (void)vm; (void)src; (void)src_len; (void)src_name;
    (void)out_buf; (void)out_len; (void)err_buf; (void)err_cap;
    return URBI_ERR_INVALID_ARG;
#endif
}
#endif /* !URBI_BYTECODE_ONLY */

#ifdef URBI_DEBUG
/* urbi_in_isr: query the registered ISR-context predicate.  Hides
 * vm->isr_check_fn so URBI_ASSERT_NOT_ISR can be written without a
 * complete struct UVM in scope.  Closes API-018 / GC-012 structurally. */
bool
urbi_in_isr(const struct UVM *vm)
{
    return vm != NULL
        && vm->isr_check_fn != NULL
        && vm->isr_check_fn();
}
#endif

/* urbi_set_callback_watchdog_mode: select WARN or ASSERT on slow callback. */
void
urbi_set_callback_watchdog_mode(struct UVM *vm, UWatchdogMode mode)
{
    if (!vm) return;
    vm->callback_watchdog_mode = (uint8_t)mode;
}

/* urbi_call_host_with_watchdog: URBI_DEBUG build implementation.
 * Times fn() using vm->host_time_us; logs or panics if elapsed exceeds
 * vm->callback_warn_us.  Non-debug builds use the macro in urbi.h.
 *
 * API-010: NULL vm or NULL fn returns urbi_make_nil() defensively rather
 * than dereferencing.  Non-debug builds use the macro form which has no
 * defensive layer — those callers are expected to validate args themselves. */
#ifdef URBI_DEBUG
UValue
urbi_call_host_with_watchdog(struct UVM *vm, struct UStrand *s,
                             UHostFn fn, int argc, UValue *argv)
{
    if (!vm || !fn) return urbi_make_nil();
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
 * UVAL_FLOAT: hashes the float bit pattern at its actual width (f32 or f64).
 * UVAL_STR: hashes the interned pointer address.  Stable within one VM
 *   lifetime (intern table never moves pointers); NOT cross-run-stable
 *   because allocator placement varies between process invocations.
 * UVAL_NIL / UVAL_VOID / UVAL_CLOSURE / UVAL_STRAND / UVAL_OBJECT /
 *   UVAL_EVENT / UVAL_HOST_FN: hash only the kind byte (already mixed
 *   above the switch).  All seven carry heap pointers (or sentinels) that
 *   are not deterministic across runs, so the payload is intentionally
 *   NOT folded.  Closes API-025: comment now matches the default arm's
 *   actual coverage. */
static void
checksum_walk_cb(struct UVM *vm, UValue *root, void *ctx)
{
    UChecksumCtx *c = (UChecksumCtx *)ctx;
    (void)vm;

    FNV1A_MIX(c->h, root->kind);
    switch (root->kind) {
        case UVAL_INT:
        case UVAL_BOOL: {
            /* Reinterpret the int payload via memcpy for symmetry with the
             * UVAL_FLOAT arm.  (uint64_t)root->v.i would also produce the
             * same bits on two's-complement (universal in C), but the
             * memcpy form is uniform across all numeric arms.  API-026. */
            uint64_t bits;
            memcpy(&bits, &root->v.i, sizeof(bits));
            FNV1A_MIX(c->h, bits);
            break;
        }
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
            /* All remaining kinds (NIL, VOID, CLOSURE, STRAND, OBJECT,
             * EVENT, HOST_FN): kind already mixed above; payload dropped. */
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

    /* 6. M4 T30 — per-IC observable state.  Walk every live UChunkInstance
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
        const struct UChunkInstance *mi;
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
                    /* Root chunk — ic_count lives on root_proto (Task 11). */
                    ic_count = (mi->module->root_proto != NULL)
                               ? mi->module->root_proto->ic_count
                               : 0U;
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
