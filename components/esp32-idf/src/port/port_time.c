/* SPDX-License-Identifier: BSD-3-Clause */
/* The clock hook (urbi_set_clock): monotonic microseconds since boot from
 * esp_timer_get_time.  The int64_t it returns is never negative, so the
 * cast is exact. */
#include <stdint.h>

#include "esp_timer.h"

#include "port_esp_idf.h"

uint64_t port_time_us(void *ud)
{
    (void)ud;
    return (uint64_t)esp_timer_get_time();
}
