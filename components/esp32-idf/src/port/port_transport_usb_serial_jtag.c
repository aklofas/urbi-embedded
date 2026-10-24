/* SPDX-License-Identifier: BSD-3-Clause */
/* The ESP32-S3's built-in USB Serial/JTAG port as a REPL transport.
 *
 * read never blocks: it returns what the driver's receive buffer holds,
 * 0 when nothing is pending, and never a negative count, because this
 * port does not close.
 *
 * The driver's write is all-or-nothing against its TX ring: a buffer
 * larger than the ring never fits, and the call returns 0 for good.  So
 * write offers at most the TX size per call and returns what the driver
 * took; the REPL service re-offers the remainder on its next step.  The
 * raw write, the console sink, must not send part of a line, so it
 * refuses (returns 0) anything longer than the TX size and otherwise
 * sends the whole buffer or nothing.  Both wait at most the deadline
 * given to init. */
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "driver/usb_serial_jtag.h"

#include "port_esp_idf.h"

static TickType_t s_write_deadline;
static size_t     s_tx_size;

int port_usb_serial_jtag_init(size_t rx_buf, size_t tx_buf, uint32_t write_deadline_ms)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = (uint32_t)tx_buf,
        .rx_buffer_size = (uint32_t)rx_buf,
    };
    s_write_deadline = pdMS_TO_TICKS(write_deadline_ms);
    s_tx_size = tx_buf;
    return usb_serial_jtag_driver_install(&cfg) == ESP_OK ? 0 : -1;
}

static int usj_write_bounded(const void *buf, size_t n)
{
    int took = usb_serial_jtag_write_bytes(buf, n, s_write_deadline);
    return took < 0 ? 0 : took;
}

int port_usb_serial_jtag_raw_write(const void *buf, size_t n)
{
    if (n > s_tx_size) return 0;
    return usj_write_bounded(buf, n);
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
    return usj_write_bounded(buf, n < s_tx_size ? n : s_tx_size);
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
