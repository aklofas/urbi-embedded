/* SPDX-License-Identifier: BSD-3-Clause */
/* URBI_REQUIRE — unconditional runtime invariant macro.
 *
 * Fires in ALL build modes: host, release (-DNDEBUG), freestanding,
 * bytecode-only.  Use for programming-error invariants that MUST be
 * diagnosed at runtime regardless of build configuration.
 *
 * On failure:
 *   - If an embedder hook has been registered via urbi_set_require_fail_hook,
 *     that hook is called and must not return.
 *   - On hosted builds (__STDC_HOSTED__) with no hook: prints to stderr
 *     then calls abort().
 *   - On freestanding builds with no hook: spins in an infinite loop
 *     (embedder SHOULD register a hook that resets the MCU).
 *
 * See docs/internals/assertion-discipline.md for the full macro comparison
 * table and "when to use which" guidance.
 *
 * Distinct from:
 *   - URBI_INTERNAL_ASSERT(cond): debug-only; strips in freestanding and
 *     in any build that doesn't define URBI_DEBUG (src/runtime/umacros.h).
 *   - URBI_DISPATCH_ASSERT(cond): debug-only; defined locally in src/vm/uvm.c
 *     and stripped in non-URBI_DEBUG builds — documented hazard per
 *     runtime-invariants audit F2.
 */

#ifndef URBI_REQUIRE_H
#define URBI_REQUIRE_H

#ifdef __cplusplus
extern "C" {
#endif

/* urbi_require_fail — called when URBI_REQUIRE(cond, msg) fires (cond false).
 * Implementation in src/runtime/urequire.c.
 * Must not return; either the hook must not return, or the default impl
 * abort()s / spins. */
void urbi_require_fail(const char *file, int line,
                       const char *cond_str, const char *msg);

/* Embedder-overridable failure hook.
 * Register before calling urbi_vm_init to ensure early failures are caught.
 * The hook MUST NOT return — it should reset the MCU, log + halt, etc.
 * Passing NULL restores the default behavior (abort on host, spin on
 * freestanding). */
typedef void (*urbi_require_fail_hook_fn)(const char *file, int line,
                                          const char *cond_str, const char *msg);
void urbi_set_require_fail_hook(urbi_require_fail_hook_fn hook);

/* URBI_REQUIRE(cond, msg) — unconditional runtime invariant check.
 *
 * Parameters:
 *   cond — boolean expression; must evaluate to nonzero when the invariant
 *           holds.  Evaluated exactly once.
 *   msg  — string literal describing what the invariant guarantees.  Shown
 *           in the failure output alongside __FILE__, __LINE__, and #cond.
 *
 * Example:
 *   URBI_REQUIRE(strand->state != STRAND_DEAD, "strand must be alive");
 */
#define URBI_REQUIRE(cond, msg) \
    do { \
        if (!(cond)) { \
            urbi_require_fail(__FILE__, __LINE__, #cond, (msg)); \
        } \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* URBI_REQUIRE_H */
