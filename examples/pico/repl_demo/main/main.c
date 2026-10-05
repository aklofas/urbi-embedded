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
 * Console: every line printed here goes to UART0 as plain text, where it
 * interleaves with the UART session's JSON; that is the human and debug
 * channel.  On USB CDC it goes out as plain text before a session exists
 * and as an NDJSON output envelope after, so every line a machine client
 * reads there is JSON (see console_begin).
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

/* The compile budget of a session, sized to the 64 KB stack.  Measured
 * through the eval service with this configuration: `1+1` alone touches
 * 16,880 B of stack, and the worst static path per expression-nesting
 * level on this core is 2,976 B, so 12 levels take 52,592 B and leave
 * about 12.9 KB of the 64 KB.  Statement and brace nesting are not
 * counted by the parser's depth budget (the emitter refuses past 32
 * nested blocks, but only after parsing); the 1 KiB source cap bounds
 * them for ordinary lines.  A 32-byte guard at the stack bottom turns
 * most overruns into a hard fault and the LED error pattern (see
 * isr_hardfault); a frame larger than 32 B can step over it, so a deep
 * enough line can still reach the heap until the parser counts
 * statement nesting.
 * An AST node is 56 bytes, so 1,000 nodes is a 56 KB transient at
 * worst, which the source cap makes unreachable in practice. */
#define BUDGET_DEPTH      12u
#define BUDGET_NODES      1000u
#define BUDGET_SOURCE     1024u

/* Every block carries its requested size in a header, since a
 * realloc-shaped allocator is not told the old size.  The header is a
 * union so it costs 8 bytes on this core while still keeping the block
 * 8-byte aligned for a double.  s_alloc_live counts requested bytes
 * only, the same figure the 32-bit probe counts and caps (the probe's
 * own header is larger; the count is the same). */
typedef union { size_t n; double align; } AllocHdr;
static size_t s_alloc_live;

static void *port_alloc(void *ptr, size_t n, void *ud)
{
    (void)ud;
    AllocHdr *h = ptr ? ((AllocHdr *)ptr - 1) : NULL;
    if (n == 0U) {
        if (h) { s_alloc_live -= h->n; free(h); }
        return NULL;
    }
    size_t old = h ? h->n : 0U;
    AllocHdr *nh = (AllocHdr *)realloc(h, sizeof(AllocHdr) + n);
    if (nh == NULL) return NULL;
    nh->n = n;
    s_alloc_live = s_alloc_live - old + n;
    return nh + 1;
}

static uint64_t clock_us(void *ud) { (void)ud; return time_us_64(); }

/* --- console ------------------------------------------------------------ */

/* One escaped byte of a JSON string into `out` (room for 6); returns how
 * many characters it wrote.  Plain C so it can be checked on its own. */
static size_t json_escape_byte(unsigned char c, char out[6])
{
    static const char hex[] = "0123456789abcdef";
    switch (c) {
    case '"':  out[0] = '\\'; out[1] = '"';  return 2;
    case '\\': out[0] = '\\'; out[1] = '\\'; return 2;
    case '\n': out[0] = '\\'; out[1] = 'n';  return 2;
    case '\r': out[0] = '\\'; out[1] = 'r';  return 2;
    case '\t': out[0] = '\\'; out[1] = 't';  return 2;
    default:
        if (c < 0x20U || c == 0x7fU) {
            out[0] = '\\'; out[1] = 'u'; out[2] = '0'; out[3] = '0';
            out[4] = hex[c >> 4]; out[5] = hex[c & 0xfU];
            return 6;
        }
        out[0] = (char)c;
        return 1;
    }
}

/* Writes all of `p` to USB CDC as the FIFO drains, pumping TinyUSB; what
 * is left when the deadline passes is dropped. */
static void cdc_write_all(const char *p, size_t n, absolute_time_t deadline)
{
    while (n > 0U) {
        uint32_t room = tud_cdc_write_available();
        if (room > 0U) {
            uint32_t w = tud_cdc_write(p, (uint32_t)(n < room ? n : room));
            (void)tud_cdc_write_flush();
            p += w;
            n -= w;
        }
        if (n == 0U) break;
        tud_task();
        if (time_reached(deadline)) break;
    }
}

/* A console line is written as begin, any number of parts, end.
 *
 * UART0 gets the plain text: it is the human and debug channel, where
 * these lines and the UART session's JSON interleave.
 *
 * USB CDC gets the plain text only before a session is registered on it
 * (the boot banner).  Once a session exists, every line on USB CDC is
 * NDJSON: the console line goes out as one output envelope on the
 * "console" channel, and only when the session has no half-written line
 * of its own in the FIFO.  If it does, the CDC copy is skipped, not
 * queued; the line still reaches UART0. */
enum { CON_UART_ONLY, CON_CDC_RAW, CON_CDC_ENVELOPE };
static int s_con_mode;
static absolute_time_t s_con_deadline;

static void console_begin(void)
{
    s_con_mode = CON_UART_ONLY;
    if (!tud_cdc_connected()) return;
    s_con_deadline = make_timeout_time_ms(100);
    if (!transport_usb_cdc_is_open()) { s_con_mode = CON_CDC_RAW; return; }
    if (transport_usb_cdc_output_pending()) return;
    s_con_mode = CON_CDC_ENVELOPE;
    static const char head[] = "{\"kind\":\"output\",\"channel\":\"console\",\"msg\":\"";
    cdc_write_all(head, sizeof head - 1U, s_con_deadline);
}

static void console_part(const char *s, size_t n)
{
    uart_write_blocking(uart0, (const uint8_t *)s, n);
    if (s_con_mode == CON_CDC_RAW) {
        cdc_write_all(s, n, s_con_deadline);
    } else if (s_con_mode == CON_CDC_ENVELOPE) {
        char buf[64];
        size_t used = 0U;
        for (size_t i = 0U; i < n; i++) {
            if (used > sizeof buf - 6U) { cdc_write_all(buf, used, s_con_deadline); used = 0U; }
            used += json_escape_byte((unsigned char)s[i], &buf[used]);
        }
        if (used > 0U) cdc_write_all(buf, used, s_con_deadline);
    }
}

static void console_end(void)
{
    if (s_con_mode == CON_CDC_ENVELOPE) cdc_write_all("\"}\n", 3U, s_con_deadline);
}

static void console_write(const char *s, size_t n)
{
    console_begin();
    console_part(s, n);
    console_end();
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
    console_begin();
    console_part(head, (size_t)n);
    console_part(msg, len);
    console_part("\r\n", 2U);
    console_end();
}

/* Three long pulses, a pause, repeat: an init step failed, as opposed to
 * a hang (no LED activity at all).  It busy-waits because the loop also
 * runs from the hard-fault handler, where no interrupt is delivered. */
__attribute__((noreturn)) static void error_loop(void)
{
    for (;;) {
        for (int i = 0; i < 3; i++) { gpio_put(LED_PIN, 1); busy_wait_ms(1500); gpio_put(LED_PIN, 0); busy_wait_ms(500); }
        busy_wait_ms(2000);
    }
}

/* The SDK's hard-fault vector.  A 32-byte guard at the stack bottom turns
 * most stack overruns into a hard fault that lands here: the board shows
 * the three-pulse error pattern and needs a reset.  A frame larger than
 * 32 B can step over the guard, so a deep enough line can still reach
 * the heap until the parser counts statement nesting. */
void isr_hardfault(void);
void isr_hardfault(void) { error_loop(); }

static void report_last_error(UVM *vm)
{
    UErrorInfo info;
    memset(&info, 0, sizeof info);
    (void)urbi_last_error(vm, &info);
    const char *m = info.message ? info.message : "(no message)";
    console_begin();
    console_part("  ", 2U);
    console_part(m, strlen(m));
    console_part("\r\n", 2U);
    console_end();
}

/* One heap line, after a full collection.  `alloc live` is the figure the
 * 32-bit probe caps: requested bytes outstanding through port_alloc.
 * `gc live` is the collector's view of what it holds.  `heap break` is
 * newlib's high-water mark, including its own chunk headers and the
 * allocator's.  Then the budget the collector paces against and the
 * cycle count. */
static void report_heap(UVM *vm, const char *label)
{
    urbi_gc_collect(vm);
    UGcStats st;
    memset(&st, 0, sizeof st);
    (void)urbi_gc_stats(vm, &st);
    size_t brk = (size_t)((char *)sbrk(0) - &end);
    char line[192];
    int n = snprintf(line, sizeof line,
                     "%s: alloc live %lu B, gc live %lu B, heap break %lu B, budget %lu B, cycles %lu\r\n",
                     label, (unsigned long)s_alloc_live, (unsigned long)st.bytes_live,
                     (unsigned long)brk, (unsigned long)st.heap_budget, (unsigned long)st.cycles);
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
         * never more than 100 ms away, so a timer resolves within that).
         * Not while UART0 output is part-way out: the TX FIFO holds 32
         * bytes and raises no interrupt, so the loop keeps sweeping until
         * the service's remainder has gone. */
        if (!transport_uart_output_pending() &&
            (st == URBI_STEP_QUIESCENT ||
             (st == URBI_STEP_IDLE_UNTIL && wake_us > now + 100000u))) {
            __wfi();
        }
    }
    return 0;
}
