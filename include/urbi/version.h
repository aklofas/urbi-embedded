/* SPDX-License-Identifier: BSD-3-Clause */
/* Public C API version macros + getter.
 *
 * Stability: core.
 *
 * Separate from URBI_BYTECODE_VERSION_BYTE (wire-format byte for .uc blobs)
 * and urbi_version() (project release string). Tracks the C-API contract
 * across MAJOR.MINOR.PATCH per the policy in CONTRIBUTING.md.
 *
 * MAJOR — removed function, changed signature, removed/renumbered enum
 *         value, struct-layout change visible across the boundary,
 *         removed URBI_ERR_* slot.
 * MINOR — additive: new function, new enum value appended, new URBI_ERR_*
 *         slot at the next free index, new build flag.
 * PATCH — bug fix only, no header change at all.
 *
 * Pre-v1.0 escape clause: while URBI_API_VERSION_MAJOR == 0, MINOR or
 * PATCH bumps MAY break ABI per standard semver convention — each bump
 * enumerates breakages in CHANGELOG. Uses to date:
 *   1. v0.7.2-esp32 — S41 urbi_set_diag_fn addition (0/7/1 → 0/7/3).
 *   2. v0.7.3-bugfixes — umodule_destroy signature change (0/7/3 → 0/7/4).
 *   3. v0.8.0-loader-strand — UModule.refcount + destroy_requested fields;
 *      urbi_run_chunk const-revert; URBI_ERR_LOADER_BUDGET addition
 *      (0/7/4 → 0/7/5).
 * Strict policy goes live at v1.0.0.
 *
 * Holding a pointer to an opaque type is part of the ABI; reading through
 * it is not.
 */

#ifndef URBI_VERSION_H
#define URBI_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

#define URBI_API_VERSION_MAJOR  0
#define URBI_API_VERSION_MINOR  7
#define URBI_API_VERSION_PATCH  5
#define URBI_API_VERSION_NUM    ((URBI_API_VERSION_MAJOR * 10000) \
                                + (URBI_API_VERSION_MINOR *   100) \
                                +  URBI_API_VERSION_PATCH)

/* Runtime getter. NULL-tolerant per arg. */
void urbi_api_version(int *out_major, int *out_minor, int *out_patch);

#ifdef __cplusplus
}
#endif

#endif /* URBI_VERSION_H */
