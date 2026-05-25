/* SPDX-License-Identifier: BSD-3-Clause */
/* ESP-IDF time source wrapper.
 *
 * Satisfies urbi_time_us_fn (include/urbi/urbi.h): returns monotonic
 * microseconds.  Install via urbi_set_time_us(vm, port_time_us).
 *
 * esp_timer_get_time() returns int64_t microseconds since boot; the cast
 * to uint64_t is safe (values are always non-negative for the lifetime of
 * the device — int64 monotonic from 0). */
#include "esp_timer.h"
#include <stdint.h>

#include "port_esp_idf.h"

uint64_t port_time_us(void *ud)
{
    (void)ud;
    return (uint64_t)esp_timer_get_time();
}
