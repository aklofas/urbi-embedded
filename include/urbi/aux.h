/* SPDX-License-Identifier: BSD-3-Clause */
/* Public convenience layer (aux).
 *
 * Stability: aux. Free to evolve within a MAJOR per <urbi/version.h>.
 *
 * Compiled into separate liburbi_aux.a archive — linker-omittable for
 * minimal-footprint or certification builds. Optional at link time.
 *
 * Governance rule: every aux function must be strictly implementable via
 * <urbi/urbi.h> public API. No private header access, no internal state
 * peeking, no performance shortcuts. Enforced at PR review (see
 * CONTRIBUTING.md "Aux layer governance"). If a proposed aux function
 * can't meet the rule, either refactor until it can, or propose the
 * addition to core (paying the cost against the < 80-fn urbi.h budget
 * per REVIVAL §6).
 *
 * v0.7.0 ships with one helper: urbi_aux_check_version. Grows organically
 * in Wave 2+ as ports (ESP-IDF, STM32) surface real convenience patterns.
 * No speculative seeding — wait for a real call site (Lua's lauxlib
 * precedent).
 */

#ifndef URBI_AUX_H
#define URBI_AUX_H

#include "urbi/types.h"   /* UErrCode for return type */
#include "urbi/version.h" /* the macros this helper checks */

#ifdef __cplusplus
extern "C" {
#endif

/* === Compile-vs-runtime ABI version mismatch check ===
 *
 * Returns URBI_OK if the runtime library matches the header version the
 * embedder compiled against, or URBI_ERR_API_VERSION_MISMATCH otherwise.
 *
 * Mechanically derivable from URBI_API_VERSION_* macros — the embedder's
 * TU bakes the macro values at compile time; the library implementation
 * compares against the runtime URBI_API_VERSION_* values. Mismatch means
 * the embedder linked against a different library version than the
 * headers they #included.
 *
 * Lua precedent: lua_aux_checkversion. */
int urbi_aux_check_version(void);

#ifdef __cplusplus
}
#endif

#endif /* URBI_AUX_H */
