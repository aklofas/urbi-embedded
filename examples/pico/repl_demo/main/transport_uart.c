/* SPDX-License-Identifier: BSD-3-Clause */
#include "transport_uart.h"

#include <stdint.h>

static int uart_rd(void *ctx, void *buf, size_t n)
{
    uart_inst_t *u = (uart_inst_t *)ctx;
    uint8_t *p = (uint8_t *)buf;
    size_t i = 0;
    while (i < n && uart_is_readable(u)) p[i++] = (uint8_t)uart_getc(u);
    return (int)i;
}

static int uart_wr(void *ctx, const void *buf, size_t n)
{
    uart_inst_t *u = (uart_inst_t *)ctx;
    const uint8_t *p = (const uint8_t *)buf;
    size_t i = 0;
    while (i < n && uart_is_writable(u)) uart_putc_raw(u, (char)p[i++]);
    return (int)i;
}

static void uart_cl(void *ctx) { (void)ctx; }

void transport_uart_vtable(uart_inst_t *uart, UTransport *out)
{
    out->ctx = uart;
    out->read = uart_rd;
    out->write = uart_wr;
    out->close = uart_cl;
}
