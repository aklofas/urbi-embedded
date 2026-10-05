/* SPDX-License-Identifier: BSD-3-Clause */
/* The eval service's USB CDC stream (TinyUSB device, polled).
 *
 * A session lives as long as the host keeps DTR asserted: the terminal
 * opening /dev/ttyACM0 raises it, closing the terminal drops it, and the
 * read callback reports end of stream the moment it is gone.  The main
 * loop registers a fresh session on the next connect, which is how the
 * same stream serves one session after another. */
#ifndef REPL_DEMO_TRANSPORT_USB_CDC_H
#define REPL_DEMO_TRANSPORT_USB_CDC_H

#include <stdbool.h>
#include "urbi/repl.h"

/* Fills `out` with the CDC vtable (ctx is unused: TinyUSB has one CDC
 * interface in this build). */
void transport_usb_cdc_vtable(UTransport *out);

/* Whether a session is currently registered on the stream.  The main
 * loop sets it after urbi_repl_register_transport succeeds; the
 * service's close callback clears it. */
void transport_usb_cdc_set_open(bool open);
bool transport_usb_cdc_is_open(void);

#endif
