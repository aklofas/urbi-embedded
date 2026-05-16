/* SPDX-License-Identifier: BSD-3-Clause */
/* ESP-IDF writer wrapper — channel-dispatching output sink.
 *
 * Satisfies urbi_writer_fn (include/urbi/urbi.h:564) — install via
 * urbi_set_writer(vm, port_writer, NULL).
 *
 * Channel routing:
 *   "cerr" -> ESP_LOGE("urbi", ...)
 *   "clog" -> ESP_LOGI("urbi", ...)
 *   other  -> printf("[<channel>] <msg>\n") for visibility on UART.
 *
 * msg is NOT NUL-terminated (urbi contract); use the %.*s precision form
 * so we never read past msg_len bytes. ts_us is the host-time hook output
 * captured at urbi_vm_write time; ESP_LOG already prefixes its own
 * timestamp so we discard it here. */
#include "esp_log.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "port_esp_idf.h"

void port_writer(void *ud,
                 const char *channel, size_t channel_len,
                 const char *msg,     size_t msg_len,
                 uint64_t ts_us)
{
    (void)ud;
    (void)ts_us;

    if (channel_len == 4 && memcmp(channel, "cerr", 4) == 0) {
        ESP_LOGE("urbi", "%.*s", (int)msg_len, msg);
    } else if (channel_len == 4 && memcmp(channel, "clog", 4) == 0) {
        ESP_LOGI("urbi", "%.*s", (int)msg_len, msg);
    } else {
        printf("[%.*s] %.*s\n", (int)channel_len, channel,
                                (int)msg_len, msg);
    }
}
