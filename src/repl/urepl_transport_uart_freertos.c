/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_transport_uart_freertos.c — FreeRTOS UART transport
 *
 * Thin wrapper around generic FreeRTOS UART driver primitives (the
 * platform's BSP provides xUartRead / xUartWrite / vUartInit).  This
 * v0.9.1 stub gives embedders a starting point for FreeRTOS-on-bare-
 * metal Cortex-M ports (e.g. STM32 with USART2 over ST-Link); the
 * real-hardware exercise lands in v0.9.2 with the Pi Pico USB CDC port.
 *
 * Gating:
 *   - URBI_ENABLE_REPL must be set (otherwise the REPL service isn't
 *     compiled in at all).
 *   - One of the FreeRTOS detection macros below — the host build does
 *     not define any of these, so this TU compiles to empty on Linux /
 *     macOS / etc.  The platform symbol UREPL_UART_FREERTOS_TRANSPORT
 *     is absent in that case and the embedder cannot link against it.
 *
 * Single-client (UART is point-to-point): accept_fn returns the
 * configured client fd once, then -1 forever after.  client_fd is a
 * platform-specific handle (often a queue handle cast through int);
 * read_fn / write_fn pass it through to the BSP. */
#include "urbi/repl.h"

#if defined(URBI_ENABLE_REPL) \
 && (defined(FREERTOS) || defined(__FREERTOS__) || defined(INC_FREERTOS_H))

#include <stdbool.h>
#include <stdlib.h>

/* The FreeRTOS BSP provides these; declared extern so this TU can be
 * compiled without the actual BSP headers in tree.  Embedders link
 * their implementation at app-link time. */
extern int  xUartRead (int handle, void *buf, unsigned n);  /* >=0 bytes, -1 EAGAIN, -errno hard */
extern int  xUartWrite(int handle, const void *buf, unsigned n);
extern void vUartFlush(int handle);

typedef struct UUartFreertosState {
    int  handle;     /* BSP handle — UART port number, queue handle, etc. */
    bool accepted;
} UUartFreertosState;

UUartFreertosState *
urepl_uart_freertos_state_create(int handle)
{
    UUartFreertosState *st =
        (UUartFreertosState *)calloc(1, sizeof(*st));
    if (st == NULL) {
        return NULL;
    }
    st->handle = handle;
    return st;
}

void
urepl_uart_freertos_state_destroy(UUartFreertosState *st)
{
    if (st == NULL) {
        return;
    }
    vUartFlush(st->handle);
    free(st);
}

static int
uart_freertos_accept(void *listener_state, int *out_client_fd)
{
    UUartFreertosState *st = (UUartFreertosState *)listener_state;
    if (st == NULL || out_client_fd == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    if (st->accepted) {
        return -1;
    }
    st->accepted = true;
    *out_client_fd = st->handle;
    return 0;
}

static int
uart_freertos_read(int client_fd, void *buf, size_t n)
{
    return xUartRead(client_fd, buf, (unsigned)n);
}

static int
uart_freertos_write(int client_fd, const void *buf, size_t n)
{
    return xUartWrite(client_fd, buf, (unsigned)n);
}

static void
uart_freertos_close(int client_fd)
{
    vUartFlush(client_fd);
}

static int
uart_freertos_pollable_fd(int client_fd)
{
    (void)client_fd;
    return -1;  /* FreeRTOS UART driver isn't pollable via poll(2); the
                   reader subthread reads via xUartRead's blocking-with-
                   timeout shape.  v0.9.2 wires a real wake source. */
}

const UTransport UREPL_UART_FREERTOS_TRANSPORT = {
    .name           = "uart-freertos",
    .accept_fn      = uart_freertos_accept,
    .read_fn        = uart_freertos_read,
    .write_fn       = uart_freertos_write,
    .close_fn       = uart_freertos_close,
    .pollable_fd_fn = uart_freertos_pollable_fd
};

#endif /* URBI_ENABLE_REPL && FreeRTOS */
