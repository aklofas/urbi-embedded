/* SPDX-License-Identifier: BSD-3-Clause */
/* isa_method.h — the isA method table.  Installed on the Object root by
 * uboot_table alongside ustdlib_object_methods. */

#ifndef URBI_STDLIB_ISA_METHOD_H
#define URBI_STDLIB_ISA_METHOD_H

#include "rt/ustdlib_glue.h"

enum { USTDLIB_ISA_NMETHODS = 1 };
extern const UMethodDef ustdlib_isa_methods[USTDLIB_ISA_NMETHODS];

#endif
