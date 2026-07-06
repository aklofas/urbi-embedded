/* SPDX-License-Identifier: BSD-3-Clause */
/* test_emit_const_index.c — for-each loop constants must keep 16-bit
 * constant-pool indices (refactor-3 FE-09).
 *
 * The for-each emitter loads two int constants (0 for the index init,
 * 1 for the increment) via urbi_emit_add_const_int, which returns uint16_t.  Both
 * call sites in uemit_stmt.c cast the result through uint8_t, silently
 * wrapping any pool index > 255 and loading the WRONG constant.  This
 * test pads the pool past index 255 with distinct int literals before a
 * for-each loop so the loop's 0/1 constants land at high indices. */

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

UTEST(foreach_const_index_above_255)
{
    /* 300 distinct int literals pad the pool past 255, then a for-each
     * whose 0/1 loop constants land at indices > 255.  The old
     * (uint8_t)urbi_emit_add_const_int casts truncated them (refactor-3 FE-09).
     * The padding values deliberately avoid 0 and 1 so the loop
     * constants cannot dedup to a low pool index.  Bare-literal
     * expression statements are used (not assignments) because each
     * global assignment consumes an IC site and 300 of them trip
     * EMIT_TOO_MANY_IC_SITES before the pool ever overflows. */
    char src[16384];
    size_t off = 0;
    for (int i = 0; i < 300; i++) {
        off += (size_t)snprintf(src + off, sizeof src - off,
                                "%d; ", 10000 + i);
    }
    off += (size_t)snprintf(src + off, sizeof src - off,
        "var s = 0; for (var x : [1, 2, 3]) { s = s + x }; s |");
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
    UASSERT_EQ(6LL, (long long)result.v.i);

    free(bc);
    urbi_vm_destroy(&vm);
}

void test_emit_const_index_suite(void) {
    utest_run("foreach_const_index_above_255", foreach_const_index_above_255);
}
