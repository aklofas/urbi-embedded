/* SPDX-License-Identifier: BSD-3-Clause */
#include "transport_usb_cdc.h"

#include "tusb.h"

static bool s_open;
/* The service's last write left part of its offer unwritten, so a line
 * of its own is half out in the FIFO. */
static bool s_pending;

/* Positive: bytes read.  Zero: nothing this instant, stream still open.
 * Negative: DTR is gone, the session ends. */
static int cdc_read(void *ctx, void *buf, size_t n)
{
    (void)ctx;
    if (!tud_cdc_connected()) return -1;
    uint32_t avail = tud_cdc_available();
    if (avail == 0U) return 0;
    if (n > avail) n = avail;
    return (int)tud_cdc_read(buf, (uint32_t)n);
}

/* Writes what the endpoint FIFO has room for and flushes; the service
 * re-offers the remainder on its next sweep.  A host that stops reading
 * makes this return 0 forever, and the service drops output past its
 * cap rather than waiting. */
static int cdc_write(void *ctx, const void *buf, size_t n)
{
    (void)ctx;
    if (!tud_cdc_connected()) return -1;
    uint32_t room = tud_cdc_write_available();
    if (room == 0U) { s_pending = n > 0U; return 0; }
    uint32_t want = n > room ? room : (uint32_t)n;
    uint32_t wrote = tud_cdc_write(buf, want);
    (void)tud_cdc_write_flush();
    s_pending = wrote < n;
    return (int)wrote;
}

static void cdc_close(void *ctx)
{
    (void)ctx;
    s_open = false;
    s_pending = false;
}

void transport_usb_cdc_vtable(UTransport *out)
{
    out->ctx = NULL;
    out->read = cdc_read;
    out->write = cdc_write;
    out->close = cdc_close;
}

void transport_usb_cdc_set_open(bool open) { s_open = open; s_pending = false; }
bool transport_usb_cdc_is_open(void) { return s_open; }
bool transport_usb_cdc_output_pending(void) { return s_pending; }
