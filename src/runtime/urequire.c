/* SPDX-License-Identifier: BSD-3-Clause */
/* src/runtime/urequire.c — URBI_REQUIRE failure handler.
 *
 * Implements the two public symbols declared in <urbi/require.h>:
 *   urbi_require_fail()          — called when URBI_REQUIRE fires
 *   urbi_set_require_fail_hook() — embedder override for freestanding
 *
 * Build-mode behavior summary:
 *   Hosted (__STDC_HOSTED__), no hook → fprintf(stderr) + abort()
 *   Hosted,  hook registered          → hook(file, line, cond, msg); return
 *   Freestanding, no hook             → infinite spin (embedder bug: should
 *                                       register a hook to MCU-reset)
 *   Freestanding, hook registered     → hook(file, line, cond, msg); return
 *   URBI_BYTECODE_ONLY=1              → stderr print omitted; spin/hook only
 *
 * The hook must not return; the caller (URBI_REQUIRE expansion) does not
 * check the return and will proceed past the call if the hook returns,
 * leading to undefined behavior at the invariant-violation site.
 */

#include "urbi/require.h"
#include <stddef.h>   /* NULL — needed in freestanding builds */

/* fprintf + abort are only available on hosted targets. */
#if __STDC_HOSTED__ && !defined(URBI_BYTECODE_ONLY)
#  include <stdio.h>
#  include <stdlib.h>
#endif

static urbi_require_fail_hook_fn g_require_hook = NULL;  /* NOLINT(cppcoreguidelines-avoid-non-const-global-variables) — mutable by design: urbi_set_require_fail_hook writes it once at startup */

void urbi_set_require_fail_hook(urbi_require_fail_hook_fn hook)
{
    g_require_hook = hook;
}

void urbi_require_fail(const char *file, int line,
                       const char *cond_str, const char *msg)
{
    /* Embedder-registered hook takes priority in all build modes. */
    if (g_require_hook) {
        g_require_hook(file, line, cond_str, msg);
        /* Hook must not return; if it does, fall through to spin below. */
    }

#if __STDC_HOSTED__ && !defined(URBI_BYTECODE_ONLY)
    /* Hosted with no hook (or hook returned): print diagnostic and abort. */
    fprintf(stderr, "URBI_REQUIRE failed: %s:%d: %s -- %s\n",
            file, line, cond_str, msg);
    abort();
#else
    /* Freestanding / bytecode-only with no hook (or hook returned):
     * spin forever.  Embedder SHOULD register a hook that resets the MCU.
     * The (void) casts suppress -Wunused-parameter in strict builds. */
    (void)file;
    (void)line;
    (void)cond_str;
    (void)msg;
    for (;;) {
        /* spin — embedder should have registered a hook */
    }
#endif
}
