/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_transport_uart_esp_idf.c — ESP-IDF UART transport
 *
 * Thin wrapper around ESP-IDF's uart_driver_install / uart_read_bytes /
 * uart_write_bytes (driver/uart.h).  Pairs with the v0.7.2 ESP32-S3-EYE
 * port, which already drives the rest of the runtime; this TU lets that
 * board (or any ESP-IDF target) expose the REPL service over a UART
 * console interface rather than only via TCP.
 *
 * Gating:
 *   - URBI_ENABLE_REPL must be set.
 *   - ESP_PLATFORM is defined by the ESP-IDF toolchain on every
 *     compile.  On host this is not set, so the TU compiles empty.
 *
 * The ESP-IDF UART driver does not expose a poll(2)-style fd; reads
 * are blocking-with-timeout via uart_read_bytes.  The reader subthread
 * polls by calling read_fn with a non-zero VFS timeout (handled inside
 * the BSP).  v0.9.2 will add a FreeRTOS-queue-backed wake source if
 * needed; for v0.9.1 the busy-loop-with-timeout pattern matches what
 * the existing eye_demo telemetry uses. */
#include "urbi/repl.h"

#if defined(URBI_ENABLE_REPL) && defined(ESP_PLATFORM)

#include <stdbool.h>
#include <stdlib.h>

#include "driver/uart.h"

typedef struct UUartEspIdfState {
    uart_port_t port;     /* UART_NUM_0 / UART_NUM_1 / UART_NUM_2 */
    bool        accepted;
} UUartEspIdfState;

UUartEspIdfState *
urepl_uart_esp_idf_state_create(uart_port_t port)
{
    UUartEspIdfState *st =
        (UUartEspIdfState *)calloc(1, sizeof(*st));
    if (st == NULL) {
        return NULL;
    }
    st->port = port;
    return st;
}

void
urepl_uart_esp_idf_state_destroy(UUartEspIdfState *st)
{
    if (st == NULL) {
        return;
    }
    (void)uart_flush(st->port);
    free(st);
}

static int
uart_esp_idf_accept(void *listener_state, int *out_client_fd)
{
    UUartEspIdfState *st = (UUartEspIdfState *)listener_state;
    if (st == NULL || out_client_fd == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    if (st->accepted) {
        return -1;
    }
    st->accepted = true;
    *out_client_fd = (int)st->port;
    return 0;
}

static int
uart_esp_idf_read(int client_fd, void *buf, size_t n)
{
    /* Non-blocking read — VFS layer returns 0 if no bytes pending. */
    int got = uart_read_bytes((uart_port_t)client_fd,
                              (uint8_t *)buf, n, 0);
    if (got < 0) {
        return -1;  /* treat any error as "would-block"; reader retries */
    }
    return got;
}

static int
uart_esp_idf_write(int client_fd, const void *buf, size_t n)
{
    int wrote = uart_write_bytes((uart_port_t)client_fd,
                                 (const char *)buf, n);
    if (wrote < 0) {
        return -1;
    }
    return wrote;
}

static void
uart_esp_idf_close(int client_fd)
{
    (void)uart_flush((uart_port_t)client_fd);
}

static int
uart_esp_idf_pollable_fd(int client_fd)
{
    (void)client_fd;
    return -1;  /* uart driver not poll(2)-able; reader spins with
                   non-blocking reads + a brief vTaskDelay. */
}

const UTransport UREPL_UART_ESP_IDF_TRANSPORT = {
    .name           = "uart-esp-idf",
    .accept_fn      = uart_esp_idf_accept,
    .read_fn        = uart_esp_idf_read,
    .write_fn       = uart_esp_idf_write,
    .close_fn       = uart_esp_idf_close,
    .pollable_fd_fn = uart_esp_idf_pollable_fd
};

#endif /* URBI_ENABLE_REPL && ESP_PLATFORM */
