/* SPDX-License-Identifier: BSD-3-Clause */
/* ESP-IDF port glue — runtime diagnostic channel adapter.
 *
 * port_diag_to_esp: shim that satisfies the urbi_diag_fn typedef
 * (include/urbi/urbi.h) and routes runtime diagnostic messages to
 * the ESP_LOG facility under a fixed "urbi-runtime" tag.
 *
 *   URBI_LOG_DEBUG / INFO  -> ESP_LOGI
 *   URBI_LOG_WARN          -> ESP_LOGW
 *   URBI_LOG_ERROR         -> ESP_LOGE
 *
 * vsnprintf into a 192-byte stack buffer; truncates silently past that.
 * The runtime's existing call sites all use fixed short strings, so the
 * buffer is plenty.  Formatted reports are reserved for the future. */
#include <stdarg.h>
#include <stdio.h>

#include "esp_log.h"
#include "urbi/urbi.h"     /* URBI_LOG_* level enum */

#include "port_esp_idf.h"  /* port_diag_to_esp prototype */

static const char *TAG = "urbi-runtime";

void
port_diag_to_esp(struct UVM *vm, void *ud, int level, const char *fmt, ...)
{
    (void)vm; (void)ud;

    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    (void)n;   /* truncation accepted silently */

    switch (level) {
    case URBI_LOG_ERROR:
        ESP_LOGE(TAG, "%s", buf);
        break;
    case URBI_LOG_WARN:
        ESP_LOGW(TAG, "%s", buf);
        break;
    case URBI_LOG_INFO:
    case URBI_LOG_DEBUG:
    default:
        ESP_LOGI(TAG, "%s", buf);
        break;
    }
}
