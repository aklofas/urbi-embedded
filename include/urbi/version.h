/* SPDX-License-Identifier: BSD-3-Clause */
/* Public C API version macros + getter.
 *
 * Separate from URBI_BYTECODE_VERSION_BYTE (wire-format byte for .uc blobs)
 * and urbi_version() (project release string).
 *
 * There is no API or ABI compatibility promise before 1.0.0. Bumped per
 * WORKFLOW.md; see docs/api-stability.md for what the 1.0.0 freeze will
 * promise. */
#ifndef URBI_VERSION_H
#define URBI_VERSION_H

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility push(default)   /* export only public-header symbols */
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define URBI_API_VERSION_MAJOR  0
#define URBI_API_VERSION_MINOR  25
#define URBI_API_VERSION_PATCH  0
#define URBI_API_VERSION_NUM    ((URBI_API_VERSION_MAJOR * 10000) \
                                + (URBI_API_VERSION_MINOR *   100) \
                                +  URBI_API_VERSION_PATCH)
#define URBI_API_VERSION_STRING "0.25.0"

/* Release version — the latest tag with its leading "v" stripped.  This
 * is what urbi_version() reports and what `urbi --version` prints; it
 * moves with every tag, unlike the API version above.  The
 * check-version-sync gate pins it against that tag. */
#define URBI_RELEASE_STRING "0.15.0-frontend"

/* Runtime getter. NULL-tolerant per arg. */
void urbi_api_version(int *out_major, int *out_minor, int *out_patch);

/* === API tier annotation macros ===
 *
 * URBI_EXPERIMENTAL — RESERVED for v1.x; do not depend on across releases.
 *                     Compiler emits a deprecation warning when used.
 *                     Suppress with -Wno-deprecated-declarations.
 *
 * URBI_ADVANCED     — Stable but non-hot-path.  Most embedders do not need
 *                     this; reach for it only when the basic API is not
 *                     sufficient.  No warning emitted; the macro is
 *                     documentation only.
 *
 * URBI_DEPRECATED   — Scheduled for removal in a future MAJOR bump.
 *                     Compiler emits a deprecation warning.  Suppress with
 *                     -Wno-deprecated-declarations.
 *
 * The macros work on GCC and Clang; on other compilers they expand to nothing
 * (no warning, no behaviour change).
 *
 * The authoritative tier manifest is docs/api-surface-tiers.md.
 * CI gate: make test-api-manifest. */
#if defined(__GNUC__) || defined(__clang__)
#  define URBI_EXPERIMENTAL  __attribute__((deprecated("RESERVED for v1.x — may change before v1.0")))
#  define URBI_ADVANCED      /* documentation only — no compiler warning */
#  define URBI_DEPRECATED    __attribute__((deprecated))
#else
#  define URBI_EXPERIMENTAL
#  define URBI_ADVANCED
#  define URBI_DEPRECATED
#endif

#ifdef __cplusplus
}
#endif

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility pop
#endif
#endif /* URBI_VERSION_H */
