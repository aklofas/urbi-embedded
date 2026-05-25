/* SPDX-License-Identifier: BSD-3-Clause */
/* Cross-strategy compile smoke test: URBI_GC_NONE header chain.
 *
 * Compiled with -DURBI_GC=URBI_GC_NONE (== -DURBI_GC=2) via the
 * test-gc-none-build Makefile target.  Verifies that:
 *   (a) ugc_none.h exists and is parseable.
 *   (b) The strategy header (ugc_none.h) can be included after urbi/gc.h.
 *   (c) The three no-op barrier inlines compile cleanly.
 *   (d) The feature-flag macros are visible (URBI_GC_HAS_FINALIZERS == 0, etc.).
 *
 * W2 note: urbi/gc.h no longer includes the strategy header directly (that
 * was the src/-prefixed leak W2 removes).  Internal callers that need
 * strategy-specific content include the strategy header from src/ separately.
 * This test file does so explicitly to exercise the URBI_GC_NONE path.
 *
 * This is a header-compilation check only (compiled with -fsyntax-only or
 * -E; no link against liburbi.a needed).  Linking a full URBI_GC_NONE
 * library is deferred to v2 (REVIVAL §2.2 lock / Row 10 §2.1).
 *
 * Row 10 acceptance #9 (URBI_GC_NONE builds cleanly at the header level).
 * T28. */

#include "urbi/gc.h"
/* W2: strategy header must now be included explicitly by internal callers. */
#include "gc/ugc_none.h"

/* Verify that the feature-flag overrides from ugc_none.h took effect. */
#if URBI_GC_HAS_FINALIZERS != 0
#  error "URBI_GC_NONE: expected URBI_GC_HAS_FINALIZERS == 0"
#endif
#if URBI_GC_HAS_PINNING != 0
#  error "URBI_GC_NONE: expected URBI_GC_HAS_PINNING == 0"
#endif
#if URBI_GC_INCREMENTAL_BARRIER != 0
#  error "URBI_GC_NONE: expected URBI_GC_INCREMENTAL_BARRIER == 0"
#endif
#if GC_PHASE_IDLE != 0
#  error "URBI_GC_NONE: expected GC_PHASE_IDLE == 0"
#endif

int main(void) { return 0; }
