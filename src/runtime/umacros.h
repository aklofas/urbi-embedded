/* SPDX-License-Identifier: BSD-3-Clause */
/* Project-wide internal macros shared across the runtime.
 * Everything under src/ is internal by definition (the public C API lives in
 * include/urbi/), so embedders never include this header.
 *
 * Note: URBI_ASSERT_NOT_ISR is intentionally NOT defined here — it lives in
 * include/urbi/urbi.h because it is part of the embedder-facing assertion
 * surface (host bridges can use it for ISR-unsafe-entry catches in debug
 * builds).  Per Wave-1 audit GC-012 the location is documented but not
 * relocated; the deeper hygiene of "public macro touching internal field"
 * carries forward to wave-3-naming. */

#ifndef UMACROS_H
#define UMACROS_H

#if __STDC_HOSTED__
#  include <assert.h>
#  define URBI_INTERNAL_ASSERT(cond) assert(cond)
#else
#  define URBI_INTERNAL_ASSERT(cond) ((void)0)
#endif

#endif /* UMACROS_H */
