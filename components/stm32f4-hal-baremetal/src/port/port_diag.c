/* SPDX-License-Identifier: BSD-3-Clause */
/* Diagnostic-channel adapter — routes urbi runtime URBI_LOG_* messages
 * to USART1 via port_writer.  vsnprintf into a fixed buffer (truncates
 * silently past 192 chars). */

#include "port_stm32f4.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define URBI_LOG_DEBUG 0
#define URBI_LOG_INFO  1
#define URBI_LOG_WARN  2
#define URBI_LOG_ERROR 3

void port_diag(struct UVM *vm, int level, const char *fmt, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = (int)(sizeof buf - 1);

    const char *prefix;
    switch (level) {
        case URBI_LOG_DEBUG: prefix = "[D]"; break;
        case URBI_LOG_INFO:  prefix = "[I]"; break;
        case URBI_LOG_WARN:  prefix = "[W]"; break;
        case URBI_LOG_ERROR: prefix = "[E]"; break;
        default:             prefix = "[?]"; break;
    }

    port_writer(vm, prefix, strlen(prefix), buf, (size_t)n, 0);
    port_writer(vm, "", 0, "\n", 1, 0);
}
