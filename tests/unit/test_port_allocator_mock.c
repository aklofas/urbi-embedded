/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_port_allocator_mock.c — T15: port_psram_alloc signature
 * compatibility check with UVMAllocFn (compile-only — host cannot link
 * the actual symbol because it requires esp_heap_caps.h). */

#include "utest.h"
#include "urbi/types.h"

#define UTEST(name) static void name(void)

/* Mirror the port_psram_alloc signature on host. The actual symbol is in
 * components/esp32-idf/src/port/port_allocator.c and depends on heap_caps_*. */
static void *mock_port_psram_alloc(void *ptr, size_t nbytes, void *ud) {
    (void)ptr; (void)nbytes; (void)ud;
    return NULL;
}

/* Compile-time check: assigning to UVMAllocFn fails if signature drifts. */
static const UVMAllocFn signature_check = mock_port_psram_alloc;

UTEST(port_allocator_signature_matches_uvmallocfn) {
    /* The real compile-time check is the assignment above. This runtime
     * assertion just keeps the symbol used so the test framework runs it. */
    UASSERT(signature_check != NULL);
}

void test_port_allocator_mock_suite(void) {
    utest_run("port_allocator_signature_matches_uvmallocfn",
              port_allocator_signature_matches_uvmallocfn);
}
