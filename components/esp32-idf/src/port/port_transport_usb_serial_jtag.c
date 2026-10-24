/* SPDX-License-Identifier: BSD-3-Clause */
/* The ESP32-S3's built-in USB Serial/JTAG port as a REPL transport.
 *
 * read never blocks: it returns what the driver's receive buffer holds,
 * 0 when nothing is pending, and never a negative count, because this
 * port does not close.  write waits at most the deadline given to init
 * and returns what the driver accepted; the REPL service re-offers a
 * short write's remainder on its next step.  The same bounded write is
 * the console sink. */
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "driver/usb_serial_jtag.h"

#include "port_esp_idf.h"

static TickType_t s_write_deadline;

int port_usb_serial_jtag_init(size_t rx_buf, size_t tx_buf, uint32_t write_deadline_ms)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = (uint32_t)tx_buf,
        .rx_buffer_size = (uint32_t)rx_buf,
    };
    s_write_deadline = pdMS_TO_TICKS(write_deadline_ms);
    return usb_serial_jtag_driver_install(&cfg) == ESP_OK ? 0 : -1;
}

int port_usb_serial_jtag_raw_write(const void *buf, size_t n)
{
    int took = usb_serial_jtag_write_bytes(buf, n, s_write_deadline);
    return took < 0 ? 0 : took;
}

static int usj_read(void *ctx, void *buf, size_t n)
{
    (void)ctx;
    int got = usb_serial_jtag_read_bytes(buf, (uint32_t)n, 0);
    return got < 0 ? 0 : got;
}

static int usj_write(void *ctx, const void *buf, size_t n)
{
    (void)ctx;
    return port_usb_serial_jtag_raw_write(buf, n);
}

static void usj_close(void *ctx)
{
    (void)ctx;
}

void port_usb_serial_jtag_vtable(UTransport *out)
{
    out->ctx = NULL;
    out->read = usj_read;
    out->write = usj_write;
    out->close = usj_close;
}
