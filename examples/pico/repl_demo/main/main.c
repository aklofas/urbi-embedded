/* SPDX-License-Identifier: BSD-3-Clause */
/* examples/pico/repl_demo/main/main.c — Pi Pico interactive REPL demo.
 *
 * Bare-metal init: pico-sdk stdlib, UART0 (115200 8N1, GP0/GP1), TinyUSB
 * device, urbi VM with realloc-style port_alloc, load + run baked
 * repl_demo workload, register BSP fixtures, register USB CDC + UART
 * REPL transports, arm TIMER_IRQ_0 (100 ms tick), drop into the
 * cooperative main loop.
 *
 * Single-core; no FreeRTOS — pico-sdk default linker script lays out
 * stack/heap such that newlib's malloc/realloc/free service requests
 * from the remaining ~200 KB of SRAM. */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>   /* malloc / realloc / free from newlib */

#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "tusb.h"

#include "urbi/urbi.h"
#include "urbi/repl.h"
#include "urbi/types.h"

/* Full UVM struct definition needed for static allocation — same pattern
 * as examples/stm32f4/mandelbrot/main.c.  The public API only forward-
 * declares the type; embedders that allocate UVM in BSS must pull the
 * internal header. */
#include "vm/uvm.h"

/* Freestanding deserialization API — bare-metal uses uchunk_deserialize
 * directly (urbi_chunk_from_bytes returns NULL on __STDC_HOSTED__ == 0). */
#include "chunk/uchunk.h"

#include "bsp/bsp_register.h"

#include "repl_demo_baked.h"   /* repl_demo[] + repl_demo_size */

/* --- Forward decls for the Pi Pico REPL transports.  These live in
 * src/repl/urepl_transport_{usb_cdc,uart}_pico.c (compiled by this
 * CMakeLists with URBI_PICO_USB_CDC=1) and aren't header-exposed
 * — same pattern as the v0.9.1 UART Pico stub. */
struct UUsbCdcPicoState;
struct UUsbCdcPicoState *urepl_usb_cdc_pico_state_create(void);
extern const UTransport UREPL_USB_CDC_PICO_TRANSPORT;

struct UUartPicoState;
struct UUartPicoState *urepl_uart_pico_state_create(unsigned instance);
extern const UTransport UREPL_UART_PICO_TRANSPORT;

/* --- Allocator shim: pico-sdk newlib provides malloc/realloc/free, but
 * urbi's UVMAllocFn / UChunkAllocFn use a single realloc-style entry
 * point with (ptr, nbytes, ud) signature.  This trivial wrapper plugs
 * the two together. */
static void *port_alloc(void *ptr, size_t nbytes, void *ud)
{
    (void)ud;
    if (nbytes == 0U) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, nbytes);
}

/* --- Statics. */
static struct UVM vm;   /* BSS — port_alloc fills the rest. */

int main(void)
{
    /* pico-sdk default init: clocks, watchdog, stdio (disabled by
     * pico_enable_stdio_{usb,uart} in CMakeLists — we drive both
     * transports ourselves). */
    stdio_init_all();

    /* UART0 for the REPL UART transport.  GP0 = TX, GP1 = RX, 115200 8N1. */
    uart_init(uart0, 115200);
    gpio_set_function(0, GPIO_FUNC_UART);
    gpio_set_function(1, GPIO_FUNC_UART);

    /* TinyUSB device-side init — handles the USB CDC ACM endpoint that
     * the USB CDC transport reads from. */
    tusb_init();

    /* urbi VM bring-up.  port_alloc handles all heap traffic. */
    if (urbi_vm_init(&vm, port_alloc, NULL) != 0) {
        const char err[] = "urbi_vm_init FAILED\r\n";
        uart_write_blocking(uart0, (const uint8_t *)err, sizeof err - 1U);
        while (1) { tight_loop_contents(); }
    }

    /* Materialize the global realm (triggers stdlib_boot lazily inside
     * urealm_globals.c → urbi_populate_realm_globals → urbi_stdlib_boot). */
    if (urbi_realm_global(&vm) == NULL) {
        const char err[] = "urbi_realm_global FAILED\r\n";
        uart_write_blocking(uart0, (const uint8_t *)err, sizeof err - 1U);
        while (1) { tight_loop_contents(); }
    }

    /* Load + run the baked workload.  Freestanding deserialize requires
     * an explicit alloc_fn (we share port_alloc with the VM). */
    UProto *root = NULL;
    char errbuf[128] = {0};
    UChunkLoadError lerr = uchunk_deserialize(&root, repl_demo, repl_demo_size,
                                              port_alloc, NULL,
                                              errbuf, sizeof errbuf);
    if (lerr != UCHUNK_LOAD_OK || root == NULL) {
        const char prefix[] = "uchunk_deserialize FAILED: ";
        uart_write_blocking(uart0, (const uint8_t *)prefix, sizeof prefix - 1U);
        const char *name = uchunk_load_error_name(lerr);
        size_t namelen = 0;
        while (name[namelen] != '\0') namelen++;
        uart_write_blocking(uart0, (const uint8_t *)name, namelen);
        uart_write_blocking(uart0, (const uint8_t *)"\r\n", 2U);
        while (1) { tight_loop_contents(); }
    }

    int rcc = urbi_run_chunk(&vm, NULL, root, NULL);
    if (rcc != 0) {
        const char err[] = "urbi_run_chunk FAILED\r\n";
        uart_write_blocking(uart0, (const uint8_t *)err, sizeof err - 1U);
        while (1) { tight_loop_contents(); }
    }

    /* BSP fixtures — must come AFTER stdlib_boot (urbi_register depends
     * on the realm-globals table being populated). */
    if (bsp_register(&vm) != 0) {
        const char err[] = "bsp_register FAILED\r\n";
        uart_write_blocking(uart0, (const uint8_t *)err, sizeof err - 1U);
        while (1) { tight_loop_contents(); }
    }

    /* REPL service — cooperative mode (both transports return -1 from
     * pollable_fd_fn, so urbi_repl_serve_step drives them).  No TCP/Unix
     * listener on bare metal. */
    UReplConfig cfg = {0};
    cfg.tcp_port           = -1;
    cfg.unix_path          = NULL;
    cfg.bind_addr          = NULL;
    cfg.auth_token         = NULL;   /* loopback-equivalent: USB/UART are physical-access only */
    cfg.max_clients        = 2;
    cfg.output_ringbuf_cap = 0U;     /* default */
    UReplServer *server = NULL;
    int rc = urbi_repl_serve_init(&vm, &cfg, &server);
    if (rc != URBI_OK || server == NULL) {
        const char err[] = "urbi_repl_serve_init FAILED\r\n";
        uart_write_blocking(uart0, (const uint8_t *)err, sizeof err - 1U);
        while (1) { tight_loop_contents(); }
    }

    /* Both transports use uart0 / TinyUSB CDC0 by convention. */
    struct UUsbCdcPicoState *cdc_state  = urepl_usb_cdc_pico_state_create();
    struct UUartPicoState   *uart_state = urepl_uart_pico_state_create(0U);
    if (cdc_state == NULL || uart_state == NULL) {
        const char err[] = "transport_state_create FAILED\r\n";
        uart_write_blocking(uart0, (const uint8_t *)err, sizeof err - 1U);
        while (1) { tight_loop_contents(); }
    }
    (void)urbi_repl_register_transport(server, &UREPL_USB_CDC_PICO_TRANSPORT, cdc_state);
    (void)urbi_repl_register_transport(server, &UREPL_UART_PICO_TRANSPORT,    uart_state);

    /* Arm TIMER_IRQ_0 — 100 ms tick + BOOTSEL polling.  After this
     * point the tick ISR fires; injected events queue into vm's event
     * ring and drain at the start of each urbi_step. */
    (void)bsp_tick_start(&vm);

    /* Hello banner over UART (USB CDC may not be enumerated yet). */
    {
        const char hello[] = "urbi v0.9.4-pico booting\r\n";
        uart_write_blocking(uart0, (const uint8_t *)hello, sizeof hello - 1U);
    }

    /* Cooperative main loop:
     *   1. tud_task              — TinyUSB device-stack housekeeping
     *   2. urbi_repl_serve_step  — accept/read/dispatch/write/close sweep
     *   3. urbi_step             — drain event ring + advance strands
     *   4. __wfi                 — sleep until the next IRQ (timer tick,
     *                              UART RX, USB SOF/CDC RX, etc.) */
    while (1) {
        tud_task();
        (void)urbi_repl_serve_step(server, 0U);
        uint64_t wake_us = 0U;
        UStepResult st = urbi_step(&vm, 256U, &wake_us);
        if (st == URBI_STEP_FATAL) {
            /* Reset on fatal — matches stm32f4 mandelbrot policy. */
            const char err[] = "urbi_step FATAL — resetting\r\n";
            uart_write_blocking(uart0, (const uint8_t *)err, sizeof err - 1U);
            watchdog_reboot(0U, 0U, 0U);
        }
        /* QUIESCENT / WAKE_AT / RUNNING all fall through to __wfi —
         * the next IRQ (tick, RX byte, USB SOF) wakes us. */
        __wfi();
    }
    return 0;
}
