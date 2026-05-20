/* SPDX-License-Identifier: BSD-3-Clause */
/* src/repl/urepl_transport_usb_cdc_pico.c — Pi Pico (RP2040/RP2350) USB CDC
 *
 * Thin wrapper around TinyUSB's CDC ACM primitives (tusb.h:
 * tud_cdc_connected / tud_cdc_available / tud_cdc_read /
 * tud_cdc_write / tud_cdc_write_flush).  Aimed at the v0.9.4-pico-example
 * port — the Pico (USB CDC and/or UART2) is the canonical on-hardware
 * validation target for the v0.9.1 remote REPL transport.
 *
 * Gating:
 *   - URBI_ENABLE_REPL must be set.
 *   - PICO_BOARD is defined by the Pico SDK's cmake on every compile.
 *   - URBI_PICO_USB_CDC is set by the embedder's CMakeLists when the
 *     USB CDC stack is wired up (tinyusb_device linked + tud_task driven
 *     from the main loop).  On host all three are unset so the TU
 *     compiles empty.
 *
 * Single-client: USB CDC has one attached host over one device interface.
 * accept_fn returns a synthetic client_fd (0) once the host has enumerated
 * the CDC interface; -1 thereafter, re-arming when the host disconnects. */
#include "urbi/repl.h"

#if defined(URBI_ENABLE_REPL) && defined(PICO_BOARD) && defined(URBI_PICO_USB_CDC)

#include <stdbool.h>
#include <stdlib.h>

#include "tusb.h"

typedef struct UUsbCdcPicoState {
    bool accepted;
} UUsbCdcPicoState;

UUsbCdcPicoState *
urepl_usb_cdc_pico_state_create(void)
{
    UUsbCdcPicoState *st = (UUsbCdcPicoState *)calloc(1, sizeof(*st));
    return st;  /* NULL on OOM; caller checks */
}

void
urepl_usb_cdc_pico_state_destroy(UUsbCdcPicoState *st)
{
    if (st == NULL) {
        return;
    }
    free(st);
}

static int
usb_cdc_pico_accept(void *listener_state, int *out_client_fd)
{
    UUsbCdcPicoState *st = (UUsbCdcPicoState *)listener_state;
    if (st == NULL || out_client_fd == NULL) {
        return URBI_ERR_INVALID_ARG;
    }
    /* Re-arm on host disconnect: clear accepted flag when CDC drops. */
    if (st->accepted && !tud_cdc_connected()) {
        st->accepted = false;
    }
    if (st->accepted) {
        return -1;  /* CDC is single-host; no further clients while connected */
    }
    if (!tud_cdc_connected()) {
        return -1;  /* no host attached yet */
    }
    st->accepted = true;
    *out_client_fd = 0;  /* synthetic — TinyUSB has one CDC interface */
    return 0;
}

static int
usb_cdc_pico_read(int client_fd, void *buf, size_t n)
{
    (void)client_fd;
    if (!tud_cdc_connected()) {
        return 0;  /* host gone — clean close, listener re-arms via accept_fn */
    }
    uint32_t avail = tud_cdc_available();
    if (avail == 0) {
        return -1;  /* would-block */
    }
    uint32_t take = (avail < (uint32_t)n) ? avail : (uint32_t)n;
    uint32_t got = tud_cdc_read(buf, take);
    if (got == 0) {
        return -1;  /* would-block (race with another reader; shouldn't
                       happen in our single-reader design) */
    }
    return (int)got;
}

static int
usb_cdc_pico_write(int client_fd, const void *buf, size_t n)
{
    (void)client_fd;
    if (!tud_cdc_connected()) {
        return -1;  /* host gone; reader will re-arm via accept_fn */
    }
    /* tud_cdc_write returns bytes accepted into TinyUSB's TX FIFO; if
     * the FIFO is full additional bytes are dropped (CDC ACM has no
     * back-pressure).  Flush to push to the host promptly. */
    uint32_t wrote = tud_cdc_write(buf, (uint32_t)n);
    (void)tud_cdc_write_flush();
    if (wrote == 0) {
        return -1;  /* would-block: FIFO full */
    }
    return (int)wrote;
}

static void
usb_cdc_pico_close(int client_fd)
{
    (void)client_fd;
    /* No-op: the BSP owns the USB stack; the embedder runs
     * urepl_usb_cdc_pico_state_destroy at shutdown. */
}

static int
usb_cdc_pico_pollable_fd(int client_fd)
{
    (void)client_fd;
    return -1;  /* TinyUSB has no poll(2); reader polls via
                   tud_cdc_available in a tud_task() loop. */
}

const UTransport UREPL_USB_CDC_PICO_TRANSPORT = {
    .name           = "usb-cdc-pico",
    .accept_fn      = usb_cdc_pico_accept,
    .read_fn        = usb_cdc_pico_read,
    .write_fn       = usb_cdc_pico_write,
    .close_fn       = usb_cdc_pico_close,
    .pollable_fd_fn = usb_cdc_pico_pollable_fd
};

#endif /* URBI_ENABLE_REPL && PICO_BOARD && URBI_PICO_USB_CDC */
