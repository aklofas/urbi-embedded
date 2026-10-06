/* SPDX-License-Identifier: BSD-3-Clause */
/* Diagnostic-channel adapter — routes urbi runtime diagnostics to USART1
 * via port_writer.  The runtime hands a finished message and a syslog
 * level (3 error, 4 warning, 6 info, 7 debug); the level picks a
 * [D]/[I]/[W]/[E] prefix and the line ends in CR LF. */

#include "port_stm32f4.h"

void port_diag(struct UVM *vm, void *ud, int level, const char *msg, size_t len) {
    (void)vm;
    (void)ud;

    const char *prefix;
    if (level <= 3)      prefix = "[E]";
    else if (level == 4) prefix = "[W]";
    else if (level <= 6) prefix = "[I]";
    else                 prefix = "[D]";

    port_writer(NULL, prefix, 3, msg, len);
    port_uart_write("\r\n", 2);
}
