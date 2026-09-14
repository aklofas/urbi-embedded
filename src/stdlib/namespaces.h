/* SPDX-License-Identifier: BSD-3-Clause */
/* namespaces.h — the System and Global method tables, and the init hook
 * that installs the constant slots (Math.pi, System.Platform.kind) the
 * boot table has no column for. */

#ifndef URBI_STDLIB_NAMESPACES_H
#define URBI_STDLIB_NAMESPACES_H

#include "rt/ustdlib_glue.h"

enum { K_SYSTEM_NMETHODS = 5 };
extern const UMethodDef k_system_methods[K_SYSTEM_NMETHODS];
enum { K_GLOBAL_NMETHODS = 1 };
extern const UMethodDef k_global_methods[K_GLOBAL_NMETHODS];

int urbi_namespaces_init(UVM *vm);

#endif
