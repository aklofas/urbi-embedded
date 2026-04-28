/* SPDX-License-Identifier: BSD-3-Clause */
/* Host type registration — row 10 §7.
 *
 * urbi_register_type is declared in ugc.h.  The T26 stub that lived in
 * ugc_incremental.c is replaced here with the real implementation. */

#include "ugc.h"
#include "uvm.h"
#include "urbi.h"          /* URBI_ASSERT_NOT_ISR */
#include "urbi_internal.h" /* URBI_INTERNAL_ASSERT */

uint8_t
urbi_register_type(UVM *vm, const UType *type)
{
    URBI_ASSERT_NOT_ISR(vm);

    uint8_t tag = type->type_tag;

    if (tag == 0u) {
        /* Auto-assign next free host slot. */
        URBI_INTERNAL_ASSERT(
            vm->host_type_count < (uint8_t)(UTYPE_HOST_MAX - UTYPE_HOST_BASE + 1u));
        tag = (uint8_t)(UTYPE_HOST_BASE + vm->host_type_count);
        vm->host_type_count++;
    } else if (tag >= UTYPE_HOST_BASE /* && tag <= UTYPE_HOST_MAX */) {
        /* Host-allocated explicit tag — use as-is. */
    } else {
        /* Tags 1..(UTYPE_HOST_BASE-1) are reserved for built-in types and
         * must not be registered via this API.
         * URBI_INTERNAL_ASSERT fires in URBI_DEBUG builds; returns 0 in release. */
        URBI_INTERNAL_ASSERT(0);
        return 0u;
    }

    vm->type_table[tag] = (UType *)type;
    return tag;
}
