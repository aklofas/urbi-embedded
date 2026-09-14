/* SPDX-License-Identifier: BSD-3-Clause */
/* primitives.h — the Mutex, Date and Duration method tables, and the
 * init hook that seeds each prototype's default state slot. */

#ifndef URBI_STDLIB_PRIMITIVES_H
#define URBI_STDLIB_PRIMITIVES_H

#include "rt/ustdlib_glue.h"

enum { K_MUTEX_NMETHODS = 5 };
extern const UMethodDef k_mutex_methods[K_MUTEX_NMETHODS];
enum { K_DATE_NMETHODS = 5 };
extern const UMethodDef k_date_methods[K_DATE_NMETHODS];
enum { K_DURATION_NMETHODS = 4 };
extern const UMethodDef k_duration_methods[K_DURATION_NMETHODS];

int urbi_primitives_init(UVM *vm);

#endif
