/* SPDX-License-Identifier: BSD-3-Clause */
/* isa_method.h — the isA method table.  Installed on the Object root by
 * uboot_table alongside k_object_methods. */

#ifndef URBI_STDLIB_ISA_METHOD_H
#define URBI_STDLIB_ISA_METHOD_H

#include "rt/ustdlib_glue.h"

enum { K_ISA_NMETHODS = 1 };
extern const UMethodDef k_isa_methods[K_ISA_NMETHODS];

#endif
