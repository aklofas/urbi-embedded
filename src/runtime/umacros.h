/* SPDX-License-Identifier: BSD-3-Clause */
/* Project-wide internal macros shared across the runtime.
 * Everything under src/ is internal by definition (the public C API lives in
 * include/urbi/), so embedders never include this header. */

#ifndef UMACROS_H
#define UMACROS_H

#if __STDC_HOSTED__
#  include <assert.h>
#  define URBI_INTERNAL_ASSERT(cond) assert(cond)
#else
#  define URBI_INTERNAL_ASSERT(cond) ((void)0)
#endif

#endif /* UMACROS_H */
