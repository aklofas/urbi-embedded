/* SPDX-License-Identifier: BSD-3-Clause */
/* Cross-strategy compile smoke test: URBI_GC_NONE header chain.
 *
 * Compiled with -DURBI_GC=URBI_GC_NONE (== -DURBI_GC=2) via the
 * test-gc-none-build Makefile target.  Verifies that:
 *   (a) ugc_none.h exists and is parseable.
 *   (b) urbi/gc.h selects ugc_none.h rather than ugc_incremental.h.
 *   (c) The three no-op barrier inlines compile cleanly.
 *   (d) The feature-flag macros are visible (URBI_GC_HAS_FINALIZERS == 0, etc.).
 *
 * This is a header-compilation check only (compiled with -fsyntax-only or
 * -E; no link against liburbi.a needed).  Linking a full URBI_GC_NONE
 * library is deferred to v2 (REVIVAL §2.2 lock / Row 10 §2.1).
 *
 * Row 10 acceptance #9 (URBI_GC_NONE builds cleanly at the header level).
 * T28. */

#include "urbi/gc.h"

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
