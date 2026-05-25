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
#include "hardware/watchdog.h"
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
#include "bsp/bsp_led.h"
#include "bsp/bsp_temp.h"
#include "bsp/bsp_button.h"
#include "bsp/bsp_tick.h"

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

/* --- C-side watcher callback for the `pressed` event.  Toggles GP25
 * directly via gpio_xor_mask.  Wired via urbi_register_watcher in main().
 *
 * Why a C-side watcher instead of urbiscript `whenever (pressed) { ... }`:
 * during v0.9.4 Pico bring-up we discovered that the workload's
 *   whenever (pressed) { led_toggle() }
 * watcher installs without error (urbi_run_chunk returns 0) but its body
 * never fires on pressed-event delivery — even synthetic urbi_inject_event
 * calls don't reach it.  The C-side urbi_register_watcher path, on the
 * other hand, fires reliably.  Filed as a v1.x design risk: "whenever
 * (named_event) install on cooperative-only builds doesn't dispatch
 * watcher body".  Once that lands, the C-side workaround can be removed
 * and the .u-file watcher will work directly. */
static int c_pressed_watcher(struct UVM *vm_arg, urbi_event_id_t event_id,
                             const UValue *args, int argc, void *ud)
{
    (void)vm_arg; (void)event_id; (void)args; (void)argc; (void)ud;
    gpio_xor_mask(1U << 25);
    return 0;
}

/* --- Distinctive error pattern: 3 long pulses (1.5 s ON, 0.5 s OFF),
 * then a 2 s OFF pause, repeating forever.  Lets the user distinguish
 * "init step errored out (silent infinite loop)" from "genuinely hung
 * mid-step (no LED activity)".  Use instead of tight_loop_contents()
 * in all init-failure paths. */
__attribute__((noreturn))
static void error_loop(void)
{
    while (1) {
        for (int i = 0; i < 3; i++) {
            gpio_put(25, 1);
            sleep_ms(1500);
            gpio_put(25, 0);
            sleep_ms(500);
        }
        sleep_ms(2000);
    }
}

/* --- Dual-channel debug print.  Writes `msg` to BOTH UART0 (115200 8N1
 * on GP0/GP1, needs USB-to-UART adapter) AND USB CDC (visible in
 * `picocom -b 115200 /dev/ttyACM0` if connected).  Pumps tud_task for
 * 30 ms after the write so the host driver actually receives the bytes
 * before we move on to the next init step.  Use during boot to surface
 * progress + failure detail richer than LED blinks. */
static void dbg_print(const char *msg)
{
    size_t n = 0;
    while (msg[n] != '\0') n++;

    /* UART0 — always works if a host adapter is connected. */
    uart_write_blocking(uart0, (const uint8_t *)msg, n);

    /* USB CDC — only useful if picocom is already attached. */
    if (tud_cdc_connected()) {
        tud_cdc_write(msg, (uint32_t)n);
        tud_cdc_write_flush();
    }

    /* Pump TinyUSB so the host driver actually drains the IN endpoint. */
    absolute_time_t deadline = make_timeout_time_ms(30);
    while (!time_reached(deadline)) {
        tud_task();
    }
}

int main(void)
{
    /* pico-sdk default init: clocks, watchdog, stdio (disabled by
     * pico_enable_stdio_{usb,uart} in CMakeLists — we drive both
     * transports ourselves). */
    stdio_init_all();

    /* Onboard LED as a heartbeat / boot-progress indicator.  GP25 on
     * Pico1.  Start OFF — every boot_blink burst transitions on→off so
     * counts are unambiguous (LED off between bursts = clean separator). */
    gpio_init(25);
    gpio_set_dir(25, GPIO_OUT);
    gpio_put(25, 0);

    /* UART0 for the REPL UART transport.  GP0 = TX, GP1 = RX, 115200 8N1. */
    uart_init(uart0, 115200);
    gpio_set_function(0, GPIO_FUNC_UART);
    gpio_set_function(1, GPIO_FUNC_UART);

    /* TinyUSB device-side init — handles the USB CDC ACM endpoint that
     * the USB CDC transport reads from. */
    tusb_init();

    /* Drive enumeration to completion BEFORE the long-running urbi init.
     * tud_task() needs to be called repeatedly during the SETUP/GET_DESC/
     * SET_CONFIG handshake.  After enumeration, the LED goes solid ON for
     * 3 s as a "connect picocom now" signal — user has that window to
     * launch picocom on /dev/ttyACM0 before boot messages start. */
    {
        absolute_time_t enum_deadline = make_timeout_time_ms(5000);
        while (!tud_mounted() && !time_reached(enum_deadline)) {
            tud_task();
        }
        /* "Connect now" signal: solid LED for 3 s.  tud_task pumped so the
         * host driver stays alive during the wait. */
        gpio_put(25, 1);
        absolute_time_t signal_deadline = make_timeout_time_ms(3000);
        while (!time_reached(signal_deadline)) {
            tud_task();
        }
        gpio_put(25, 0);
    }

    dbg_print("\r\n\r\n=== urbi v0.9.4-pico boot ===\r\n");

    dbg_print("[1] urbi_vm_init... ");
    if (urbi_vm_init(&vm, port_alloc, NULL) != 0) {
        dbg_print("FAILED\r\n");
        error_loop();
    }
    dbg_print("ok\r\n");

    dbg_print("[2] urbi_realm_global... ");
    if (urbi_realm_global(&vm) == NULL) {
        dbg_print("FAILED\r\n");
        error_loop();
    }
    dbg_print("ok\r\n");

    /* BSP fixtures — must come AFTER stdlib_boot (urbi_register depends
     * on the realm-globals table being populated) AND BEFORE
     * urbi_run_chunk because the baked workload (repl_demo.u) references
     * led_on/led_off/temp_celsius/button_pressed/pressed in at + whenever
     * watchers; running the chunk before these are registered leaves
     * dangling references that crash on the next urbi call.  Split into
     * 4 sub-steps with distinct LED counts to pinpoint which sub-register
     * hangs if the boot doesn't progress past here. */
    dbg_print("[3] bsp_led_register... ");
    if (bsp_led_register(&vm) != 0) { dbg_print("FAILED\r\n"); error_loop(); }
    dbg_print("ok\r\n");

    dbg_print("[4] bsp_temp_register... ");
    if (bsp_temp_register(&vm) != 0) { dbg_print("FAILED\r\n"); error_loop(); }
    dbg_print("ok\r\n");

    dbg_print("[5] bsp_button_register... ");
    if (bsp_button_register(&vm) != 0) { dbg_print("FAILED\r\n"); error_loop(); }
    dbg_print("ok\r\n");

    dbg_print("[6] bsp_tick_register... ");
    if (bsp_tick_register(&vm) != 0) { dbg_print("FAILED\r\n"); error_loop(); }
    dbg_print("ok\r\n");

    dbg_print("[7] uchunk_deserialize... ");
    UProto *root = NULL;
    char errbuf[128] = {0};
    UChunkLoadError lerr = uchunk_deserialize(&root, repl_demo, repl_demo_size,
                                              port_alloc, NULL,
                                              errbuf, sizeof errbuf);
    if (lerr != UCHUNK_LOAD_OK || root == NULL) {
        dbg_print("FAILED: ");
        dbg_print(uchunk_load_error_name(lerr));
        dbg_print("\r\n");
        error_loop();
    }
    dbg_print("ok\r\n");

    dbg_print("[8] urbi_run_chunk... ");
    int rcc = urbi_run_chunk(&vm, NULL, root, NULL);
    if (rcc != 0) {
        dbg_print("FAILED rc=-2 (STRAND_FATAL)\r\n");
        /* STRAND_FATAL details land in vm->last_error / last_errmsg, NOT
         * the public urbi_last_error ring (that's for API-call errors). */
        dbg_print("  vm.last_error = ");
        dbg_print(uvm_error_name(vm.last_error));
        dbg_print("\r\n  vm.last_errmsg = '");
        dbg_print(vm.last_errmsg[0] != '\0' ? vm.last_errmsg : "(empty)");
        dbg_print("'\r\n");
        error_loop();
    }
    dbg_print("ok\r\n");

    /* v0.9.4 deferral: interactive REPL service is SKIPPED on Pico.
     * The v0.9.1 per-session model creates a fresh realm per session +
     * compiles lobby.u into it, which costs >50 KB of heap on top of
     * stdlib_boot's ~165 KB baseline.  Pico's ~225 KB usable heap is
     * insufficient; we OOM on first session creation.
     *
     * Filed as v1.x design risk: "REPL session model needs lightweight
     * variant (shared-global-realm session, or stdlib_boot stripping
     * for embedded) to fit ~256 KB SRAM targets."  See
     * docs/urbi-embedded-design-risks.md.
     *
     * For this v0.9.4 demo, the workload's `whenever (pressed)` watcher
     * provides the interactive surface: pressing BOOTSEL toggles the LED.
     * Future v0.9.5+ build can re-enable urbi_repl_serve_init once the
     * lightweight session model lands. */
    dbg_print("[9-10] REPL service SKIPPED (v1.x: needs lightweight session)\r\n");

    /* Arm TIMER_IRQ_0 — 100 ms tick + BOOTSEL polling.  The tick ISR
     * injects `tick` events; BOOTSEL polls inject `pressed` events when
     * the button transitions held.  The workload's
     *   whenever (pressed) { led_toggle(); echo("button pressed") }
     * fires on each press — that's the demo's interactive surface. */
    dbg_print("[11] bsp_tick_start... ");
    (void)bsp_tick_start(&vm);
    dbg_print("ok\r\n");

    /* DEBUG: install a C-side watcher for `pressed` that directly
     * gpio_xor's the LED.  Parallel to the urbiscript `whenever (pressed)
     * { led_toggle() }` install — if the C watcher fires but the
     * urbiscript one doesn't, the bug is in `whenever` compile/install. */
    dbg_print("[12] urbi_register_watcher (C side)... ");
    urbi_event_id_t pressed_evt = bsp_button_get_pressed_evt();
    if (pressed_evt == URBI_EVENT_ID_INVALID) {
        dbg_print("FAILED (pressed_evt INVALID)\r\n");
        error_loop();
    }
    struct URealm *gr = urbi_realm_global(&vm);
    urbi_watcher_handle_t wh = urbi_register_watcher(&vm, gr, pressed_evt,
                                                     c_pressed_watcher, NULL);
    if (wh == URBI_WATCHER_HANDLE_INVALID) {
        dbg_print("FAILED (handle INVALID)\r\n");
        error_loop();
    }
    dbg_print("ok\r\n");

    dbg_print("\r\n=== boot complete; entering main loop ===\r\n");
    dbg_print("Press BOOTSEL on the Pico to toggle the LED.\r\n");
    dbg_print("(Interactive REPL deferred — see design-risks v1.x.)\r\n\r\n");

    /* LED stays OFF entering main loop — under urbiscript control after
     * this point (via the whenever(pressed) led_toggle() watcher). */
    gpio_put(25, 0);

    /* Cooperative main loop:
     *   1. tud_task   — TinyUSB device-stack housekeeping (keeps the
     *                   boot console + future CDC traffic alive)
     *   2. urbi_step  — drain event ring + advance strands; this is
     *                   what delivers `pressed` events to the C watcher
     *   3. __wfi      — sleep until the next IRQ (timer tick / USB SOF /
     *                   UART RX) */
    while (1) {
        tud_task();
        uint64_t wake_us = 0U;
        UStepResult st = urbi_step(&vm, 256U, &wake_us);
        if (st == URBI_STEP_FATAL) {
            /* Reset on fatal — matches stm32f4 mandelbrot policy. */
            const char err[] = "urbi_step FATAL — resetting\r\n";
            uart_write_blocking(uart0, (const uint8_t *)err, sizeof err - 1U);
            watchdog_reboot(0U, 0U, 0U);
        }
        __wfi();
    }
    return 0;
}
