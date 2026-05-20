/* SPDX-License-Identifier: BSD-3-Clause */
/* Opcode-allocation smoke tests for M5 reactive runtime.
 *
 * Each test verifies that the expected integer value was assigned to the
 * opcode name.  Failing here means a future task edited the enum without
 * updating the bytecode version or the disassembler — fix those first. */

#include "utest.h"
#include "chunk/uchunk.h"

static void op_at_install_allocated(void) {
    UASSERT_EQ((int)OP_AT_INSTALL, 38);
}

static void op_at_sync_install_allocated(void) {
    UASSERT_EQ((int)OP_AT_SYNC_INSTALL, 39);
}

static void op_whenever_install_allocated(void) {
    UASSERT_EQ((int)OP_WHENEVER_INSTALL, 40);
}

static void op_waituntil_install_allocated(void) {
    UASSERT_EQ((int)OP_WAITUNTIL_INSTALL, 41);
}

static void op_at_event_install_allocated(void) {
    UASSERT_EQ((int)OP_AT_EVENT_INSTALL, 42);
}

static void op_at_event_sync_install_allocated(void) {
    UASSERT_EQ((int)OP_AT_EVENT_SYNC_INSTALL, 43);
}

static void op_getslot_change_event_allocated(void) {
    UASSERT_EQ((int)OP_GETSLOT_CHANGE_EVENT, 44);
}

static void op_load_realm_global_allocated(void) {
    UASSERT_EQ((int)OP_LOAD_REALM_GLOBAL, 45);
}

void test_op_allocation_suite(void) {
    utest_run("op_at_install_allocated",          op_at_install_allocated);
    utest_run("op_at_sync_install_allocated",     op_at_sync_install_allocated);
    utest_run("op_whenever_install_allocated",    op_whenever_install_allocated);
    utest_run("op_waituntil_install_allocated",   op_waituntil_install_allocated);
    utest_run("op_at_event_install_allocated",    op_at_event_install_allocated);
    utest_run("op_at_event_sync_install_allocated", op_at_event_sync_install_allocated);
    utest_run("op_getslot_change_event_allocated", op_getslot_change_event_allocated);
    utest_run("op_load_realm_global_allocated",   op_load_realm_global_allocated);
}
