/* SPDX-License-Identifier: BSD-3-Clause */
/* The diag hook (urbi_set_diag): the runtime hands a finished message
 * and a syslog level (3 error, 4 warning, 6 info, 7 debug); the level
 * picks a [E]/[W]/[I]/[D] prefix and the message goes to the console as
 * one framed line ending in CR LF. */
#include <stddef.h>

#include "port_esp_idf.h"

void port_diag(struct UVM *vm, void *ud, int level, const char *msg, size_t len)
{
    (void)vm;
    (void)ud;

    const char *prefix;
    if (level <= 3)      prefix = "[E] ";
    else if (level == 4) prefix = "[W] ";
    else if (level <= 6) prefix = "[I] ";
    else                 prefix = "[D] ";

    port_console_begin();
    port_console_part(prefix, 4);
    port_console_part(msg, len);
    port_console_part("\r\n", 2);
    port_console_end();
}
