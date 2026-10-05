/* SPDX-License-Identifier: BSD-3-Clause */
/* The eval service's UART stream.  A UART has no notion of a connection,
 * so the session registered on it at boot lives for the life of the
 * firmware.  Reads drain a ring the RX interrupt fills; writes take what
 * the TX FIFO has room for.  Neither blocks. */
#ifndef REPL_DEMO_TRANSPORT_UART_H
#define REPL_DEMO_TRANSPORT_UART_H

#include "hardware/uart.h"
#include "urbi/repl.h"

/* Fills `out` with a vtable over `uart` (already initialised), and arms
 * that UART's RX interrupt, which feeds the ring from then on.  One UART
 * per build: the ring and the handler are shared. */
void transport_uart_vtable(uart_inst_t *uart, UTransport *out);

#endif
