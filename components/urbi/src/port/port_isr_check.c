/* SPDX-License-Identifier: BSD-3-Clause */
/* ESP-IDF ISR-context predicate wrapper.
 *
 * Satisfies the predicate type taken by urbi_set_isr_check_fn
 * (include/urbi/urbi.h:837): `bool (*fn)(void)` returning true when the
 * caller is in interrupt context.  Install via:
 *     urbi_set_isr_check_fn(vm, port_in_isr);
 *
 * xPortInIsrContext() is the canonical FreeRTOS predicate on Xtensa/RISC-V
 * (returns pdTRUE when the CPU is servicing an ISR, including nested
 * dispatch).  The BaseType_t -> bool conversion is intentional and well-
 * defined; any non-zero value maps to true. */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

#include "port_esp_idf.h"

bool port_in_isr(void)
{
    return xPortInIsrContext() != 0;
}
