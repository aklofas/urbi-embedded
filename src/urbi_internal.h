/* SPDX-License-Identifier: BSD-3-Clause */
/* Internal runtime assertion macros — NOT part of the public API.
 * Source files inside src/ may include this header; embedders must not. */

#ifndef URBI_INTERNAL_H
#define URBI_INTERNAL_H

#if __STDC_HOSTED__
#  include <assert.h>
#  define URBI_INTERNAL_ASSERT(cond) assert(cond)
#else
#  define URBI_INTERNAL_ASSERT(cond) ((void)0)
#endif

#endif /* URBI_INTERNAL_H */
