/* SPDX-License-Identifier: BSD-3-Clause */
/* primitives.h — the Mutex, Date and Duration method tables, and the
 * init hook that seeds each prototype's default state slot. */

#ifndef URBI_STDLIB_PRIMITIVES_H
#define URBI_STDLIB_PRIMITIVES_H

#include "rt/ustdlib_glue.h"

enum { USTDLIB_MUTEX_NMETHODS = 5 };
extern const UMethodDef ustdlib_mutex_methods[USTDLIB_MUTEX_NMETHODS];
enum { USTDLIB_DATE_NMETHODS = 5 };
extern const UMethodDef ustdlib_date_methods[USTDLIB_DATE_NMETHODS];
enum { USTDLIB_DURATION_NMETHODS = 4 };
extern const UMethodDef ustdlib_duration_methods[USTDLIB_DURATION_NMETHODS];

int urbi_primitives_init(UVM *vm);

#endif
