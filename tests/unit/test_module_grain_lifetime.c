/* SPDX-License-Identifier: BSD-3-Clause */
/* test_module_grain_lifetime — Variant B module-grain lifetime.
 *
 * v0.9.0-repl update: urbi_repl_eval now heap-allocates one UModule per REPL
 * line (CHSTR-027 close-out).  Modules persist in realm->loaded_protos_head
 * until urbi_realm_destroy; they are NOT destroyed (and thus NOT rescued onto
 * vm->rescued_protos) during the eval loop.
 *
 * The bounded-overhead invariant is now expressed as:
 *   (a) Structural: realm->loaded_protos_head accumulates exactly N user modules
 *       after N urbi_repl_eval calls (one heap-alloc UModule per chunk).
 *   (b) Allocation: per-iteration GC allocation < 8 KB (same bound as before;
 *       the module shell is ~400 bytes, not tracked by the GC allocator).
 *
 * The old vm->rescued_protos check is deleted — with heap-alloc modules staying
 * in the realm, rescued_protos remains empty during the eval loop.
 *
 * ASan/UBSan in CI verify the no-leak / no-UB contract across all iterations.
 */

#include "utest.h"

#include "urbi/urbi.h"
#include "urbi/gc.h"
#include "vm/uvm.h"
#include "chunk/umodule.h"
#include "realm/urealm.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define UTEST(name) static void name(void)

/* Count user modules in realm->loaded_protos_head (excludes vm->stdlib_module).
 * v0.9.0-repl: each urbi_repl_eval call leaves one heap-alloc UModule in the
 * realm list; this replaces the old vm->rescued_protos count. */
static size_t
count_user_modules(const UVM *vm, const URealm *realm)
{
    size_t count = 0;
    const UModule *m = realm->loaded_protos_head;
    while (m != NULL) {
        if (m != vm->stdlib_module) count++;
        m = m->next_in_realm;
    }
    return count;
}

/* Run one urbi_repl_eval line into realm; return URBI_OK or abort on error. */
static int
repl(UVM *vm, URealm *realm, const char *src)
{
    char buf[256];
    return urbi_repl_eval(vm, realm, src, strlen(src), buf, sizeof(buf));
}

/* -------------------------------------------------------------------------
 * Case 1: single closure escaped per chunk.
 *
 * Each iteration: "var f<i> = function() { <i> }"
 * Expected: N modules in realm after N iterations (one heap-alloc UModule per
 * chunk).  Also: per-iteration GC allocation < 8 KB.
 * ------------------------------------------------------------------------- */
UTEST(single_closure_escape_bounded)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    size_t baseline = urbi_gc_bytes_allocated_inline(&vm);

    const int N = 50;
    char src[128];
    for (int i = 0; i < N; i++) {
        snprintf(src, sizeof(src), "var f%d = function() { %d };", i, i);
        UASSERT_EQ(URBI_OK, repl(&vm, r, src));
    }

    size_t after = urbi_gc_bytes_allocated_inline(&vm);

    /* Structural invariant: exactly N user modules in realm (one per chunk).
     * v0.9.0-repl: heap-alloc modules persist in realm->loaded_protos_head;
     * vm->rescued_protos is not used during the eval loop. */
    size_t list_len = count_user_modules(&vm, r);
    UASSERT_EQ((long long)list_len, (long long)N);

    /* Allocation bound: per-iteration GC allocation < 8 KB.
     * Empirically measured at ~3100 bytes/iteration at N=100 on x86-64
     * (root_proto + nested function proto + UClosure GC cell + realm-global
     * slot entry, plus REPL pipeline overhead that is freed per-call).
     * The bound detects quadratic growth (which would exceed 8 KB rapidly)
     * while allowing for the fixed O(1)-per-iteration structural cost. */
    size_t per_iter = (after - baseline) / (size_t)N;
    UASSERT((per_iter < 8192U));
    if (per_iter >= 8192U) {
        printf("  [module_grain_lifetime] per_iter=%zu bytes (limit 8192)\n",
               per_iter);
    }

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* -------------------------------------------------------------------------
 * Case 2: multiple closures escaped per chunk.
 *
 * Each iteration escapes TWO closures from the same chunk: "var a<i> = ...,
 * var b<i> = ...".  Despite two closures, only ONE UModule is allocated per
 * chunk (the heap-alloc granularity is per urbi_repl_eval call).
 *
 * Expected: list_len == N (not 2*N).
 * ------------------------------------------------------------------------- */
UTEST(multi_closure_per_chunk_still_one_rescue)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    const int N = 30;
    char src[256];
    for (int i = 0; i < N; i++) {
        snprintf(src, sizeof(src),
            "var a%d = function() { %d }; var b%d = function() { %d };",
            i, i, i, i + 1000);
        UASSERT_EQ(URBI_OK, repl(&vm, r, src));
    }

    /* One heap-alloc UModule per urbi_repl_eval call regardless of how many
     * closures that chunk exposes. */
    size_t list_len = count_user_modules(&vm, r);
    UASSERT_EQ((long long)list_len, (long long)N);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* -------------------------------------------------------------------------
 * Case 3: all chunks — with or without escaping closures — produce one module.
 *
 * Interleave: even iterations escape a closure, odd iterations are pure
 * arithmetic.  With heap-alloc modules, ALL N iterations produce one UModule
 * in the realm (the module shell persists regardless of closure-escape).
 *
 * Expected: list_len == N (all iterations, not just the even ones).
 * ------------------------------------------------------------------------- */
UTEST(no_escape_no_rescue)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    URealm *r = urbi_realm_create(&vm);
    UASSERT(r != NULL);

    const int N = 40;
    char src[128];
    for (int i = 0; i < N; i++) {
        if (i % 2 == 0) {
            /* Escapes a closure. */
            snprintf(src, sizeof(src), "var h%d = function() { %d };", i, i);
        } else {
            /* Pure arithmetic — no surviving closure. */
            snprintf(src, sizeof(src), "var x%d = %d + 1;", i, i);
        }
        UASSERT_EQ(URBI_OK, repl(&vm, r, src));
    }

    /* All iterations produce one heap-alloc UModule in the realm. */
    size_t list_len = count_user_modules(&vm, r);
    UASSERT_EQ((long long)list_len, (long long)N);

    urbi_realm_destroy(&vm, r);
    urbi_vm_destroy(&vm);
}

/* -------------------------------------------------------------------------
 * Suite registration
 * ------------------------------------------------------------------------- */
void
test_module_grain_lifetime_suite(void)
{
    printf("test_module_grain_lifetime\n");
    utest_run("module_grain_lifetime: single_closure_escape_bounded",
              single_closure_escape_bounded);
    utest_run("module_grain_lifetime: multi_closure_per_chunk_still_one_rescue",
              multi_closure_per_chunk_still_one_rescue);
    utest_run("module_grain_lifetime: no_escape_no_rescue",
              no_escape_no_rescue);
}
