/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_transport_uart_pico.c — Pi Pico (RP2040/RP2350) UART
 *
 * Thin wrapper around Raspberry Pi Pico SDK uart primitives
 * (hardware/uart.h: uart_is_readable / uart_getc / uart_putc /
 * uart_write_blocking).  Aimed at the v0.9.2-pico-example port — the
 * Pico (USB CDC and/or UART2) is the canonical on-hardware validation
 * target for the v0.9.1 remote REPL transport.
 *
 * Gating:
 *   - URBI_ENABLE_REPL must be set.
 *   - PICO_BOARD is defined by the Pico SDK's cmake on every compile.
 *     On host this is unset so the TU compiles empty.
 *
 * client_fd carries the Pico SDK uart instance number (0 = uart0,
 * 1 = uart1); read_fn / write_fn re-wrap to the SDK's typed handle
 * via the uart_get_instance() helper.  Single-client (UART is point-
 * to-point); accept_fn returns the configured uart number once, then
 * -1 forever after. */
#include "urbi/repl.h"

#if defined(URBI_ENABLE_REPL) && defined(PICO_BOARD)

#include "repl/urepl_transport_common.h"

#include <stdbool.h>
#include <stdlib.h>

#include "hardware/uart.h"

typedef struct UUartPicoState {
    uint instance;   /* 0 or 1 — passed to uart_get_instance() */
    bool accepted;
} UUartPicoState;

UUartPicoState *
urepl_uart_pico_state_create(uint instance)
{
    UUartPicoState *st = (UUartPicoState *)calloc(1, sizeof(*st));
    if (st == NULL) {
        return NULL;
    }
    st->instance = instance;
    return st;
}

void
urepl_uart_pico_state_destroy(UUartPicoState *st)
{
    if (st == NULL) {
        return;
    }
    free(st);
}

static int
uart_pico_accept(void *listener_state, int *out_client_fd)
{
    UUartPicoState *st = (UUartPicoState *)listener_state;
    UREPL_ACCEPT_ONCE(st, out_client_fd, (int)st->instance);
}

static int
uart_pico_read(int client_fd, void *buf, size_t n)
{
    uart_inst_t *u = uart_get_instance((uint)client_fd);
    size_t i = 0;
    char *out = (char *)buf;
    while (i < n && uart_is_readable(u)) {
        out[i++] = (char)uart_getc(u);
    }
    if (i == 0) {
        return -1;  /* would-block */
    }
    return (int)i;
}

static int
uart_pico_write(int client_fd, const void *buf, size_t n)
{
    uart_inst_t *u = uart_get_instance((uint)client_fd);
    /* uart_write_blocking is blocking until the byte hits the FIFO —
     * acceptable since UART bytes flush at line rate and this is
     * called on the reader subthread which already serializes. */
    uart_write_blocking(u, (const uint8_t *)buf, n);
    return (int)n;
}

static void
uart_pico_close(int client_fd)
{
    (void)client_fd;
    /* No-op: the BSP owns the uart hardware; the embedder runs
     * urepl_uart_pico_state_destroy at shutdown. */
}

static int
uart_pico_pollable_fd(int client_fd)
{
    (void)client_fd;
    return -1;  /* Pico SDK has no poll(2); reader polls via
                   uart_is_readable in a sleep_ms loop. */
}

const UTransport UREPL_UART_PICO_TRANSPORT = {
    .name           = "uart-pico",
    .accept_fn      = uart_pico_accept,
    .read_fn        = uart_pico_read,
    .write_fn       = uart_pico_write,
    .close_fn       = uart_pico_close,
    .pollable_fd_fn = uart_pico_pollable_fd
};

#endif /* URBI_ENABLE_REPL && PICO_BOARD */
