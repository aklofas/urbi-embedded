/* SPDX-License-Identifier: BSD-3-Clause */
/* The eval service's UART stream.  A UART has no notion of a connection,
 * so the session registered on it at boot lives for the life of the
 * firmware; reads and writes are the FIFO's contents, never blocking. */
#ifndef REPL_DEMO_TRANSPORT_UART_H
#define REPL_DEMO_TRANSPORT_UART_H

#include "hardware/uart.h"
#include "urbi/repl.h"

/* Fills `out` with a vtable over `uart` (already initialised). */
void transport_uart_vtable(uart_inst_t *uart, UTransport *out);

#endif
