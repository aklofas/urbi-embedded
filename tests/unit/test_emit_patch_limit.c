/* SPDX-License-Identifier: BSD-3-Clause */
/* test_emit_patch_limit.c — fixed-capacity patch lists must latch
 * EMIT_PATCH_LIST_FULL instead of silently dropping jumps (refactor-3
 * FE-06).
 *
 * Three saturable structures share the emitter's sticky-error contract:
 *   - ULoopCtx.break_pcs[UEMIT_LOOP_PATCH_MAX = 16] — the 17th break in
 *     one loop used to silently no-op (its placeholder JMP was never
 *     patched, so it jumped 0 instructions forward);
 *   - ULoopCtx.continue_pcs[16] — same for continue;
 *   - emit_switch_arm's exit_jmps[64] — the 65th case's exit JMP kept
 *     its placeholder offset and fell into the next case's dispatch,
 *     silently executing a second case body on duplicate values.
 *
 * The specific error code is asserted through urbi_compile_source's
 * error buffer, which embeds uemit_error_name(e.error) ("emit error:
 * EMIT_PATCH_LIST_FULL"); the UEmitter struct itself is local to
 * urbi_compile_source and not reachable from this harness. */

#include "utest.h"
#include "urbi/aux.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* Build "var n = 0; while (true) { n = n + 1; if (n == 1) { break };
 * ... if (n == <arms>) { break } }; n" into src.  Returns the length. */
static size_t build_break_loop(char *src, size_t cap, int arms)
{
    size_t off = 0;
    off += (size_t)snprintf(src + off, cap - off,
                            "var n = 0; while (true) { n = n + 1");
    for (int i = 1; i <= arms; i++) {
        off += (size_t)snprintf(src + off, cap - off,
                                "; if (n == %d) { break }", i);
    }
    off += (size_t)snprintf(src + off, cap - off, " }; n");
    return off;
}

UTEST(loop_17_breaks_latches_patch_list_full)
{
    /* 17 break sites in one while body: one past UEMIT_LOOP_PATCH_MAX.
     * Before FE-06 this compiled; the 17th break's JMP stayed at its
     * placeholder offset (a no-op fallthrough). */
    char src[2048];
    size_t off = build_break_loop(src, sizeof src, 17);
    UASSERT(off < sizeof src);

    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    unsigned char *bc = NULL;
    size_t bc_len = 0;
    char err[256] = {0};
    int rc = urbi_compile_source(&vm, src, off, "test",
                                 &bc, &bc_len, err, sizeof err);
    UASSERT(rc != URBI_OK);
    UASSERT(bc == NULL);
    UASSERT(strstr(err, "EMIT_PATCH_LIST_FULL") != NULL);

    urbi_vm_destroy(&vm);
}

UTEST(loop_16_breaks_compiles_and_runs)
{
    /* Exactly UEMIT_LOOP_PATCH_MAX break sites: boundary must still
     * compile, and the FIRST break must work (loop exits with n == 1). */
    char src[2048];
    size_t off = build_break_loop(src, sizeof src, 16);
    UASSERT(off < sizeof src);

    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    unsigned char *bc = NULL;
    size_t bc_len = 0;
    char err[256] = {0};
    int rc = urbi_compile_source(&vm, src, off, "test",
                                 &bc, &bc_len, err, sizeof err);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT(bc != NULL);

    UValue result = urbi_make_nil();
    rc = urbi_aux_load_and_run(&vm, bc, bc_len, &result);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)result.kind);
    UASSERT_EQ(1LL, (long long)result.v.i);

    free(bc);
    urbi_vm_destroy(&vm);
}

UTEST(loop_17_continues_latches_patch_list_full)
{
    /* Same cap on the continue_pcs[] list — the symmetric latch site. */
    char src[2048];
    size_t off = 0;
    off += (size_t)snprintf(src + off, sizeof src - off,
                            "var i = 0; while (i < 3) { i = i + 1");
    for (int k = 1; k <= 17; k++) {
        off += (size_t)snprintf(src + off, sizeof src - off,
                                "; if (i == %d) { continue }", 100 + k);
    }
    off += (size_t)snprintf(src + off, sizeof src - off, " }; i");
    UASSERT(off < sizeof src);

    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    unsigned char *bc = NULL;
    size_t bc_len = 0;
    char err[256] = {0};
    int rc = urbi_compile_source(&vm, src, off, "test",
                                 &bc, &bc_len, err, sizeof err);
    UASSERT(rc != URBI_OK);
    UASSERT(bc == NULL);
    UASSERT(strstr(err, "EMIT_PATCH_LIST_FULL") != NULL);

    urbi_vm_destroy(&vm);
}

UTEST(switch_65_cases_latches_patch_list_full)
{
    /* 65 case bodies: one past exit_jmps[64].  Before FE-06 this
     * compiled with the 65th exit JMP unpatched (offset 0 — fell into
     * the next case's dispatch).  65 cases stay well clear of every
     * other cap: ~131 constants (64 K pool), 65 SETSLOT IC sites (256
     * cap), case blocks open/close sequentially so nesting depth stays
     * ~2 (UFS_MAX_BLOCKS = 32). */
    char src[4096];
    size_t off = 0;
    off += (size_t)snprintf(src + off, sizeof src - off,
                            "var r = 0; switch (1) {");
    for (int i = 1; i <= 65; i++) {
        off += (size_t)snprintf(src + off, sizeof src - off,
                                "%s case %d: { r = %d }",
                                (i == 1 ? "" : ";"), i, i);
    }
    off += (size_t)snprintf(src + off, sizeof src - off, " }; r");
    UASSERT(off < sizeof src);

    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    unsigned char *bc = NULL;
    size_t bc_len = 0;
    char err[256] = {0};
    int rc = urbi_compile_source(&vm, src, off, "test",
                                 &bc, &bc_len, err, sizeof err);
    UASSERT(rc != URBI_OK);
    UASSERT(bc == NULL);
    UASSERT(strstr(err, "EMIT_PATCH_LIST_FULL") != NULL);

    urbi_vm_destroy(&vm);
}

UTEST(switch_64_cases_compiles_and_runs)
{
    /* Exactly 64 case bodies: boundary must still compile, and dispatch
     * to the last case must work. */
    char src[4096];
    size_t off = 0;
    off += (size_t)snprintf(src + off, sizeof src - off,
                            "var r = 0; switch (64) {");
    for (int i = 1; i <= 64; i++) {
        off += (size_t)snprintf(src + off, sizeof src - off,
                                "%s case %d: { r = %d }",
                                (i == 1 ? "" : ";"), i, i);
    }
    off += (size_t)snprintf(src + off, sizeof src - off, " }; r");
    UASSERT(off < sizeof src);

    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    unsigned char *bc = NULL;
    size_t bc_len = 0;
    char err[256] = {0};
    int rc = urbi_compile_source(&vm, src, off, "test",
                                 &bc, &bc_len, err, sizeof err);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT(bc != NULL);

    UValue result = urbi_make_nil();
    rc = urbi_aux_load_and_run(&vm, bc, bc_len, &result);
    UASSERT_EQ(URBI_OK, rc);
    UASSERT_EQ((int)UVAL_INT, (int)result.kind);
    UASSERT_EQ(64LL, (long long)result.v.i);

    free(bc);
    urbi_vm_destroy(&vm);
}

void test_emit_patch_limit_suite(void) {
    utest_run("loop_17_breaks_latches_patch_list_full",
              loop_17_breaks_latches_patch_list_full);
    utest_run("loop_16_breaks_compiles_and_runs",
              loop_16_breaks_compiles_and_runs);
    utest_run("loop_17_continues_latches_patch_list_full",
              loop_17_continues_latches_patch_list_full);
    utest_run("switch_65_cases_latches_patch_list_full",
              switch_65_cases_latches_patch_list_full);
    utest_run("switch_64_cases_compiles_and_runs",
              switch_64_cases_compiles_and_runs);
}
