/* SPDX-License-Identifier: BSD-3-Clause */
/* examples/pico/repl_demo/main/main.c — the Pico eval-service demo.
 *
 * Boot: UART0 (GP0 TX / GP1 RX, 115200 8N1) and TinyUSB come up first so
 * every later line has somewhere to go; the VM is opened with newlib's
 * heap behind a realloc-shaped allocator and a budget the collector paces
 * against; the board verbs and events are installed; the baked workload
 * runs in the main realm; the eval service starts with a UART session
 * registered for the life of the firmware and a USB CDC session
 * registered whenever a host holds DTR; the 100 ms tick is armed last.
 *
 * Loop: pump TinyUSB, sweep the service, step the VM for one budget,
 * sweep again, then wait for an interrupt when nothing is runnable.
 *
 * Console: every line printed here goes to UART0 and, when a host is
 * attached, to USB CDC as well, interleaved with the session's JSON
 * lines.  A machine client on CDC ignores lines that do not start with
 * `{`; a person in picocom reads both.
 *
 * The wake hook (urbi_set_wake) is not installed: this loop polls at
 * least every 100 ms because of the tick, and USB and UART traffic
 * arrive as interrupts of their own. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"
#include "tusb.h"

#include "urbi/urbi.h"
#include "urbi/repl.h"

#include "bsp/bsp_register.h"
#include "transport_uart.h"
#include "transport_usb_cdc.h"
#include "repl_demo_baked.h"

/* newlib's heap runs from `end` to __StackLimit (memmap_repl_demo.ld). */
extern char end;
extern char __StackLimit;

/* What the collector may plan on: the heap minus what the eval service
 * and newlib itself take outside the VM's allocator (a session's 4 KB
 * output staging, its input line, the service's own structs). */
#define SESSION_RESERVE   (16u * 1024u)
#define STEP_BUDGET       256u
#define LED_PIN           25u
#define IDLE_REPORT_US    (30u * 1000u * 1000u)

/* The compile budget of a session.  One nesting level of the parser
 * costs about 1 KB of stack on this core and the stack is 32 KB, so 24
 * levels leave 8 KB for everything else; an AST node is 56 bytes, so
 * 2,000 nodes is a 112 KB transient at worst, which the source cap of
 * 4 KB makes unreachable in practice. */
#define BUDGET_DEPTH      24u
#define BUDGET_NODES      2000u
#define BUDGET_SOURCE     4096u

static void *port_alloc(void *ptr, size_t n, void *ud)
{
    (void)ud;
    if (n == 0U) { free(ptr); return NULL; }
    return realloc(ptr, n);
}

static uint64_t clock_us(void *ud) { (void)ud; return time_us_64(); }

/* --- console ------------------------------------------------------------ */

static void console_write(const char *s, size_t n)
{
    uart_write_blocking(uart0, (const uint8_t *)s, n);
    if (tud_cdc_connected()) {
        (void)tud_cdc_write(s, (uint32_t)n);
        (void)tud_cdc_write_flush();
        absolute_time_t deadline = make_timeout_time_ms(30);
        while (!time_reached(deadline)) tud_task();
    }
}
static void console_puts(const char *s) { console_write(s, strlen(s)); }

static void vm_writer(void *ud, const char *chan, size_t chan_len, const char *msg, size_t msg_len)
{
    (void)ud; (void)chan; (void)chan_len;
    console_write(msg, msg_len);
}

static void vm_diag(UVM *vm, void *ud, int level, const char *msg, size_t len)
{
    (void)vm; (void)ud;
    char head[24];
    int n = snprintf(head, sizeof head, "[diag %d] ", level);
    console_write(head, (size_t)n);
    console_write(msg, len);
    console_puts("\r\n");
}

/* Three long pulses, a pause, repeat: an init step failed, as opposed to
 * a hang (no LED activity at all). */
__attribute__((noreturn)) static void error_loop(void)
{
    for (;;) {
        for (int i = 0; i < 3; i++) { gpio_put(LED_PIN, 1); sleep_ms(1500); gpio_put(LED_PIN, 0); sleep_ms(500); }
        sleep_ms(2000);
    }
}

static void report_last_error(UVM *vm)
{
    UErrorInfo info;
    (void)urbi_last_error(vm, &info);
    console_puts("  ");
    console_puts(info.message ? info.message : "(no message)");
    console_puts("\r\n");
}

/* The number the definition of done reads: collector-live bytes after a
 * full collection, beside newlib's heap break (its high-water mark, the
 * figure comparable to the qemu probe's allocator count), the budget and
 * the cycle count. */
static void report_heap(UVM *vm, const char *label)
{
    urbi_gc_collect(vm);
    UGcStats st;
    memset(&st, 0, sizeof st);
    (void)urbi_gc_stats(vm, &st);
    size_t brk = (size_t)((char *)sbrk(0) - &end);
    char line[160];
    int n = snprintf(line, sizeof line,
                     "%s: live %lu B, heap break %lu B, budget %lu B, cycles %lu\r\n",
                     label, (unsigned long)st.bytes_live, (unsigned long)brk,
                     (unsigned long)st.heap_budget, (unsigned long)st.cycles);
    console_write(line, (size_t)n);
}

int main(void)
{
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 0);

    uart_init(uart0, 115200);
    gpio_set_function(0, GPIO_FUNC_UART);
    gpio_set_function(1, GPIO_FUNC_UART);

    tusb_init();
    /* Enumerate before the long init, then hold the LED on for three
     * seconds: the window to start picocom before the banner prints. */
    {
        absolute_time_t enumerate_by = make_timeout_time_ms(5000);
        while (!tud_mounted() && !time_reached(enumerate_by)) tud_task();
        gpio_put(LED_PIN, 1);
        absolute_time_t signal_until = make_timeout_time_ms(3000);
        while (!time_reached(signal_until)) tud_task();
        gpio_put(LED_PIN, 0);
    }

    console_puts("\r\n\r\n=== urbi ");
    console_puts(urbi_version());
    console_puts(" on Raspberry Pi Pico ===\r\n");

    size_t heap_bytes = (size_t)(&__StackLimit - &end);
    UVMConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.step_budget = STEP_BUDGET;
    cfg.boot_stdlib = 1;
    cfg.heap_budget = heap_bytes > SESSION_RESERVE ? heap_bytes - SESSION_RESERVE : heap_bytes;

    console_puts("[1] urbi_open... ");
    UVM *vm = urbi_open(port_alloc, NULL, &cfg);
    if (vm == NULL || urbi_realm_main(vm) == NULL) { console_puts("FAILED\r\n"); error_loop(); }
    urbi_set_clock(vm, clock_us, NULL);
    urbi_set_writer(vm, vm_writer, NULL);
    urbi_set_diag(vm, vm_diag, NULL);
    console_puts("ok\r\n");
    report_heap(vm, "boot heap");          /* the board's 32-bit boot number */

    console_puts("[2] fixtures... ");
    if (bsp_register(vm) != URBI_OK) { console_puts("FAILED\r\n"); report_last_error(vm); error_loop(); }
    console_puts("ok\r\n");

    console_puts("[3] workload... ");
    {
        UValue out = urbi_make_nil();
        int rc = urbi_load(vm, urbi_realm_main(vm), repl_demo_wire, repl_demo_wire_len, &out);
        if (rc != URBI_OK) { console_puts("FAILED\r\n"); report_last_error(vm); error_loop(); }
    }
    console_puts("ok\r\n");

    console_puts("[4] eval service... ");
    UReplConfig rcfg;
    memset(&rcfg, 0, sizeof rcfg);
    rcfg.output_buf_cap = 4096;
    rcfg.default_budget.max_parser_depth = BUDGET_DEPTH;
    rcfg.default_budget.max_ast_nodes    = BUDGET_NODES;
    rcfg.default_budget.max_source_bytes = BUDGET_SOURCE;
    UReplServer *srv = NULL;
    if (urbi_repl_serve_init(vm, &rcfg, &srv) != URBI_OK) { console_puts("FAILED\r\n"); error_loop(); }
    {
        UTransport uart_t;
        transport_uart_vtable(uart0, &uart_t);
        if (urbi_repl_register_transport(srv, &uart_t) != URBI_OK) { console_puts("FAILED (uart)\r\n"); error_loop(); }
    }
    console_puts("ok\r\n");

    console_puts("[5] tick... ");
    if (bsp_tick_start(vm) != 0) { console_puts("FAILED\r\n"); error_loop(); }
    console_puts("ok\r\n");
    report_heap(vm, "ready");

    console_puts("Sessions: USB CDC (this port) and UART0.  One JSON request per line, e.g.\r\n");
    console_puts("  {\"id\":1,\"op\":\"eval\",\"code\":\"1+1\"}\r\n");

    uint64_t next_idle_report = time_us_64() + IDLE_REPORT_US;
    for (;;) {
        tud_task();

        bool cdc_was_open = transport_usb_cdc_is_open();
        if (!cdc_was_open && tud_cdc_connected()) {
            UTransport t;
            transport_usb_cdc_vtable(&t);
            if (urbi_repl_register_transport(srv, &t) == URBI_OK) {
                transport_usb_cdc_set_open(true);
                cdc_was_open = true;
                report_heap(vm, "session open");
            }
        }

        urbi_repl_serve_step(srv, 0);
        uint64_t wake_us = 0;
        int st = urbi_step(vm, STEP_BUDGET, &wake_us);
        if (st < 0) {
            console_puts("urbi_step failed; resetting\r\n");
            report_last_error(vm);
            sleep_ms(100);
            watchdog_reboot(0U, 0U, 0U);
        }
        urbi_repl_serve_step(srv, 0);

        if (cdc_was_open && !transport_usb_cdc_is_open()) report_heap(vm, "session closed");

        uint64_t now = time_us_64();
        if (now >= next_idle_report) {
            report_heap(vm, "idle");
            next_idle_report = now + IDLE_REPORT_US;
        }

        /* Nothing runnable: sleep until the next interrupt (the tick is
         * never more than 100 ms away, so a timer resolves within that). */
        if (st == URBI_STEP_QUIESCENT ||
            (st == URBI_STEP_IDLE_UNTIL && wake_us > now + 100000u)) {
            __wfi();
        }
    }
    return 0;
}
