/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_set_wake_fn.c — T43: urbi_set_wake_fn fires on inject.
 *
 * Tests:
 *  1. Wake callback fires after urbi_inject_event from main thread.
 *  2. Wake callback fires after urbi_inject_event from simulated ISR context
 *     (isr_check_fn installed; urbi_inject_event must still fire wake_fn).
 *  3. Custom ud pointer threaded through unchanged.
 *  4. NULL wake_fn — urbi_inject_event still succeeds; no callback fires.
 */

#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include "vm/uvm.h"
#include "event/uevent_ring.h"

#include <stdint.h>
#include <stddef.h>

#define UTEST(name) static void name(void)

/* ---- helpers ----------------------------------------------------------- */

static int   g_wake_count;
static void *g_wake_ud;

static void counting_wake(void *ud)
{
    g_wake_count++;
    g_wake_ud = ud;
}

/* ISR predicate that always returns true — simulates ISR context. */
static bool always_in_isr(void) { return true; }

static void reset_wake(void)
{
    g_wake_count = 0;
    g_wake_ud    = NULL;
}

/* ---- T43-1: wake fires from main thread on successful inject ----------- */
UTEST(wake_fires_on_inject_main)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(rc, URBI_OK);

    reset_wake();
    urbi_set_wake_fn(&vm, counting_wake, NULL);

    rc = urbi_inject_event(&vm, 1U, NULL, 0U);
    UASSERT_EQ(rc, URBI_OK);

    UASSERT_EQ(g_wake_count, 1);

    /* Second inject → second wake. */
    rc = urbi_inject_event(&vm, 2U, NULL, 0U);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ(g_wake_count, 2);

    urbi_vm_destroy(&vm);
}

/* ---- T43-2: wake fires even when ISR predicate returns true ------------ */
UTEST(wake_fires_from_isr_context)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(rc, URBI_OK);

    /* Simulate ISR context — isr_check_fn returns true. */
    urbi_set_isr_check_fn(&vm, always_in_isr);

    reset_wake();
    urbi_set_wake_fn(&vm, counting_wake, NULL);

    /* urbi_inject_event is ISR-safe; wake_fn must still fire. */
    rc = urbi_inject_event(&vm, 42U, NULL, 0U);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ(g_wake_count, 1);

    urbi_vm_destroy(&vm);
}

/* ---- T43-3: ud pointer threaded through unchanged ---------------------- */
UTEST(wake_ud_pointer_threaded)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(rc, URBI_OK);

    int sentinel = 99;
    reset_wake();
    urbi_set_wake_fn(&vm, counting_wake, &sentinel);

    rc = urbi_inject_event(&vm, 7U, NULL, 0U);
    UASSERT_EQ(rc, URBI_OK);
    UASSERT_EQ(g_wake_count, 1);
    UASSERT(g_wake_ud == (void *)&sentinel);

    urbi_vm_destroy(&vm);
}

/* ---- T43-4: NULL wake_fn — inject succeeds; no callback fires ---------- */
UTEST(wake_null_fn_no_callback)
{
    UVM vm;
    int rc = urbi_vm_init(&vm, NULL, NULL);
    UASSERT_EQ(rc, URBI_OK);

    /* Install then remove. */
    urbi_set_wake_fn(&vm, counting_wake, NULL);
    urbi_set_wake_fn(&vm, NULL, NULL);

    reset_wake();

    rc = urbi_inject_event(&vm, 9U, NULL, 0U);
    UASSERT_EQ(rc, URBI_OK);       /* inject still succeeds */
    UASSERT_EQ(g_wake_count, 0);   /* no callback */

    urbi_vm_destroy(&vm);
}

/* ---- suite entry point ------------------------------------------------- */
void
test_set_wake_fn_suite(void)
{
    printf("test_set_wake_fn\n");
    utest_run("wake_fires_on_inject_main",    wake_fires_on_inject_main);
    utest_run("wake_fires_from_isr_context",  wake_fires_from_isr_context);
    utest_run("wake_ud_pointer_threaded",     wake_ud_pointer_threaded);
    utest_run("wake_null_fn_no_callback",     wake_null_fn_no_callback);
}
