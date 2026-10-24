/* SPDX-License-Identifier: BSD-3-Clause */
/* The writer hook (urbi_set_writer): everything the runtime prints goes
 * to the console as one framed line.  The channel name is dropped; a
 * session's own output reaches its client through the REPL service, not
 * through here. */
#include <stddef.h>

#include "port_esp_idf.h"

void port_writer(void *ud, const char *chan, size_t chan_len, const char *msg, size_t msg_len)
{
    (void)ud;
    (void)chan;
    (void)chan_len;
    port_console_write(msg, msg_len);
}
