/* SPDX-License-Identifier: BSD-3-Clause */
#include "transport_uart.h"

#include <stdbool.h>
#include <stdint.h>

#include "hardware/irq.h"

/* Received bytes, moved out of the 32-byte hardware FIFO by the RX
 * interrupt so nothing is lost while the main loop sleeps or runs the
 * VM.  Single producer (the interrupt writes s_head) and single consumer
 * (uart_rd writes s_tail); a byte that arrives with the ring full is
 * dropped. */
#define RX_RING 256u
static uint8_t s_ring[RX_RING];
static volatile uint32_t s_head;
static volatile uint32_t s_tail;
static uart_inst_t *s_uart;

static void uart_rx_irq(void)
{
    while (uart_is_readable(s_uart)) {
        uint8_t c = (uint8_t)uart_getc(s_uart);
        uint32_t h = s_head;
        if (h - s_tail < RX_RING) {
            s_ring[h % RX_RING] = c;
            s_head = h + 1U;
        }
    }
}

static int uart_rd(void *ctx, void *buf, size_t n)
{
    (void)ctx;
    uint8_t *p = (uint8_t *)buf;
    size_t i = 0;
    uint32_t t = s_tail;
    while (i < n && t != s_head) p[i++] = s_ring[t++ % RX_RING];
    s_tail = t;
    return (int)i;
}

/* The service's last write left part of its offer unwritten. */
static bool s_tx_pending;

static int uart_wr(void *ctx, const void *buf, size_t n)
{
    uart_inst_t *u = (uart_inst_t *)ctx;
    const uint8_t *p = (const uint8_t *)buf;
    size_t i = 0;
    while (i < n && uart_is_writable(u)) uart_putc_raw(u, (char)p[i++]);
    s_tx_pending = i < n;
    return (int)i;
}

static void uart_cl(void *ctx) { (void)ctx; }

void transport_uart_vtable(uart_inst_t *uart, UTransport *out)
{
    s_uart = uart;
    int irq = uart == uart0 ? UART0_IRQ : UART1_IRQ;
    irq_set_exclusive_handler(irq, uart_rx_irq);
    irq_set_enabled(irq, true);
    uart_set_irq_enables(uart, true, false);

    out->ctx = uart;
    out->read = uart_rd;
    out->write = uart_wr;
    out->close = uart_cl;
}

bool transport_uart_output_pending(void) { return s_tx_pending; }
