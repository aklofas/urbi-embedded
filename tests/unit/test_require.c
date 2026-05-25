/* SPDX-License-Identifier: BSD-3-Clause */
/* tests/unit/test_require.c — URBI_REQUIRE macro + hook API */

#include "utest.h"
#include "urbi/require.h"
#include <setjmp.h>
#include <string.h>

#define UTEST(name) static void name(void)

/* --- hook infrastructure -------------------------------------------------- */

static jmp_buf   g_jb;
static const char *g_captured_msg;
static int        g_hook_call_count;

static void capture_hook(const char *file, int line,
                         const char *cond_str, const char *msg)
{
    (void)file; (void)line; (void)cond_str;
    g_captured_msg = msg;
    g_hook_call_count++;
    longjmp(g_jb, 1);
}

/* Reset helpers so each test starts clean. */
static void reset_hook_state(void)
{
    g_captured_msg    = NULL;
    g_hook_call_count = 0;
    urbi_set_require_fail_hook(capture_hook);
}

static void clear_hook(void)
{
    urbi_set_require_fail_hook(NULL);
    g_captured_msg    = NULL;
    g_hook_call_count = 0;
}

/* --- tests ----------------------------------------------------------------- */

UTEST(require_fires_on_false) {
    reset_hook_state();
    if (setjmp(g_jb) == 0) {
        URBI_REQUIRE(1 == 2, "deliberate fail");
        UASSERT(0 && "should not reach here");
    }
    UASSERT_STR_EQ(g_captured_msg, "deliberate fail");
    UASSERT_EQ(g_hook_call_count, 1);
    clear_hook();
}

UTEST(require_does_not_fire_on_true) {
    reset_hook_state();
    /* If the macro fires incorrectly, the hook will longjmp and the
     * assertion below will never run — the suite will report the test
     * as failing due to the setjmp result check in the runner. */
    URBI_REQUIRE(1 == 1, "should not fire");
    URBI_REQUIRE(42 > 0, "should not fire either");
    UASSERT(g_captured_msg == NULL);
    UASSERT_EQ(g_hook_call_count, 0);
    clear_hook();
}

UTEST(require_hook_receives_correct_message) {
    reset_hook_state();
    if (setjmp(g_jb) == 0) {
        URBI_REQUIRE(0, "specific message for hook test");
        UASSERT(0 && "unreachable");
    }
    UASSERT_STR_EQ(g_captured_msg, "specific message for hook test");
    clear_hook();
}

UTEST(require_null_hook_restore) {
    /* Verify that passing NULL to urbi_set_require_fail_hook restores
     * default behavior (nothing we can easily observe without abort, so
     * we just verify the function does not crash and subsequent hook
     * registrations work correctly). */
    urbi_set_require_fail_hook(NULL);
    /* Re-register and exercise. */
    reset_hook_state();
    if (setjmp(g_jb) == 0) {
        URBI_REQUIRE(0, "post-null-restore");
        UASSERT(0 && "unreachable");
    }
    UASSERT_STR_EQ(g_captured_msg, "post-null-restore");
    UASSERT_EQ(g_hook_call_count, 1);
    clear_hook();
}

UTEST(require_fires_exactly_once_per_violation) {
    reset_hook_state();
    if (setjmp(g_jb) == 0) {
        URBI_REQUIRE(0, "count check");
        UASSERT(0 && "unreachable");
    }
    UASSERT_EQ(g_hook_call_count, 1);
    clear_hook();
}

/* --- suite registration ---------------------------------------------------- */

void test_require_suite(void)
{
    utest_run("require_fires_on_false",
              require_fires_on_false);
    utest_run("require_does_not_fire_on_true",
              require_does_not_fire_on_true);
    utest_run("require_hook_receives_correct_message",
              require_hook_receives_correct_message);
    utest_run("require_null_hook_restore",
              require_null_hook_restore);
    utest_run("require_fires_exactly_once_per_violation",
              require_fires_exactly_once_per_violation);
}
