/* SPDX-License-Identifier: BSD-3-Clause */
/* Public convenience-layer implementation. Compiled into liburbi_aux.a
 * (separate archive) per CONTRIBUTING.md "Aux layer governance". */

#include "urbi/aux.h"
#include "urbi/types.h"
#include "urbi/version.h"

int urbi_aux_check_version(void) {
    int rt_major = 0, rt_minor = 0, rt_patch = 0;
    urbi_api_version(&rt_major, &rt_minor, &rt_patch);
    if (rt_major != URBI_API_VERSION_MAJOR
     || rt_minor != URBI_API_VERSION_MINOR
     || rt_patch != URBI_API_VERSION_PATCH) {
        return URBI_ERR_API_VERSION_MISMATCH;
    }
    return URBI_OK;
}
