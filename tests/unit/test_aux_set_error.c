/* SPDX-License-Identifier: BSD-3-Clause */
/* test_aux_set_error.c — TDD tests for urbi_aux_set_error printf helper
 * and the public urbi_set_error backing function (Phase 9, v0.7.1).
 *
 * Sub-tests:
 *   1. format_stored_in_ring: urbi_aux_set_error formats message correctly
 *      and the result lands in the error ring with the right code, message,
 *      source_name, and source_line.
 *   2. format_truncates_safely: very long message is truncated without
 *      buffer overrun; NUL termination guaranteed.
 *   3. null_vm_noop: NULL vm silently does nothing. */

#include "utest.h"
#include "urbi/aux.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"

#include <string.h>
#include <stddef.h>
#include <stdint.h>

#define UTEST(name) static void name(void)

/* =========================================================================
 * Sub-test 1: formatted message appears in error ring with correct fields.
 * ========================================================================= */

UTEST(format_stored_in_ring)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    urbi_clear_error(&vm);

    urbi_aux_set_error(&vm, -42, "foo.urbi", 10, "bad arg %d", 5);

    urbi_error_info_t info;
    int code = urbi_last_error(&vm, &info);

    UASSERT_EQ(-42, code);
    UASSERT(strcmp(info.message, "bad arg 5") == 0);
    UASSERT(strcmp(info.source_name, "foo.urbi") == 0);
    UASSERT_EQ(10, info.source_line);
    /* context is set to "urbi_aux_set_error" by the implementation */
    UASSERT(strcmp(info.context, "urbi_aux_set_error") == 0);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 2: very long format string truncates without crash.
 * ========================================================================= */

UTEST(format_truncates_safely)
{
    UVM vm;
    UASSERT_EQ(URBI_OK, urbi_vm_init(&vm, NULL, NULL));

    urbi_clear_error(&vm);

    /* Build a long format string by repeating "XXXXXXXX" (8 chars) 100 times
     * — total 800 chars, well past the 255-char ring buffer limit. */
    char long_msg[801];
    size_t i;
    for (i = 0; i < 100; i++) {
        const char *seg = "XXXXXXXX";
        size_t j;
        for (j = 0; j < 8; j++) long_msg[i * 8 + j] = seg[j];
    }
    long_msg[800] = '\0';

    /* Use %s to force the full string through vsnprintf. */
    urbi_aux_set_error(&vm, -1, "src.urbi", 1, "%s", long_msg);

    urbi_error_info_t info;
    int code = urbi_last_error(&vm, &info);

    UASSERT_EQ(-1, code);
    /* Message must be NUL-terminated and shorter than 256 chars. */
    size_t msglen = strlen(info.message);
    UASSERT(msglen < 256);
    UASSERT(msglen > 0);

    urbi_vm_destroy(&vm);
}

/* =========================================================================
 * Sub-test 3: NULL vm is a silent no-op.
 * ========================================================================= */

UTEST(null_vm_noop)
{
    /* Should not crash. */
    urbi_aux_set_error(NULL, -1, "f.u", 1, "msg %d", 0);
    /* No assertion — success means no crash. */
}

/* =========================================================================
 * Suite entry point.
 * ========================================================================= */

void
test_aux_set_error_suite(void)
{
    utest_run("aux_set_error: formatted message stored in error ring",
              format_stored_in_ring);
    utest_run("aux_set_error: long message truncates safely",
              format_truncates_safely);
    utest_run("aux_set_error: NULL vm is a no-op",
              null_vm_noop);
}
