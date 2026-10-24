/* SPDX-License-Identifier: BSD-3-Clause */
/* The ESP32-S3-EYE demo: a blob tracker in urbiscript over a camera, a
 * display and a button in C, with a REPL session on the USB Serial/JTAG
 * port.  One task owns the VM; the camera task and the button interrupt
 * post to a queue this task drains, so the runtime sees one producer.
 * Every line on the port is one JSON object: the session's replies, and
 * console text as output envelopes. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "sdkconfig.h"
#include "urbi/urbi.h"
#include "urbi/repl.h"
#include "port_esp_idf.h"
#include "eye_camera.h"
#include "eye_display.h"
#include "eye_button.h"
#include "detect_blob_chroma.h"
#include "eye_clamp.h"
#include "eye_demo_baked.h"

#define STEP_BUDGET      256u
#define QUEUE_DEPTH      32u
#define LOOP_CAP_MS      10u
#define BUDGET_DEPTH     32u
#define BUDGET_NODES     10000u
#define BUDGET_SOURCE    8192u
#define USJ_RX_BYTES     1024u
#define USJ_TX_BYTES     8192u   /* = the session's output_buf_cap: a reply frame is never split around a console line */
#define USJ_DEADLINE_MS  100u
/* From the qemu smoke: a deep parse left 41,096 B of a 65,536 B stack, so
 * 24,440 B used; rounded up to 4 KB (24 KB) plus an 8 KB margin.  Floor 16 KB. */
#define URBI_TASK_STACK  (32u * 1024u)
#define ARENA_BYTES      ((size_t)CONFIG_URBI_ARENA_BYTES)

static int c_draw_crosshair(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{ (void)vm; (void)self; (void)nargs; display_set_crosshair(eye_clamp240((int)urbi_value_as_int(args[0])), eye_clamp240((int)urbi_value_as_int(args[1]))); *out = urbi_make_nil(); return UEXEC_OK; }
static int c_set_target_chroma(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{ (void)vm; (void)self; (void)nargs; chroma_target_t t = { (uint8_t)urbi_value_as_int(args[0]), (uint8_t)urbi_value_as_int(args[1]), (uint8_t)urbi_value_as_int(args[2]) }; eye_camera_set_target(t); *out = urbi_make_nil(); return UEXEC_OK; }
static int c_log(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{ (void)vm; (void)self; (void)nargs; size_t len = 0; const char *s = urbi_value_as_str(args[0], &len); port_console_begin(); port_console_part(s ? s : "", s ? len : 0); port_console_part("\r\n", 2); port_console_end(); *out = urbi_make_nil(); return UEXEC_OK; }
static int c_heap_live(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{ (void)vm; (void)self; (void)args; (void)nargs; *out = urbi_make_int((int64_t)port_alloc_live_bytes()); return UEXEC_OK; }
static int c_events_dropped(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{ (void)vm; (void)self; (void)args; (void)nargs; *out = urbi_make_int((int64_t)port_queue_dropped() + (int64_t)port_queue_inject_failures()); return UEXEC_OK; }

/* Register an event and publish it on Object under the same name. */
static int bind_event(UVM *vm, const char *name, urbi_event_id_t *id)
{
    int rc = urbi_event_register(vm, NULL, name, id);
    if (rc != URBI_OK) return rc;
    UValue ev = urbi_make_nil(), object = urbi_make_nil();
    rc = urbi_event_value(vm, *id, &ev);
    if (rc != URBI_OK) return rc;
    rc = urbi_global_get(vm, NULL, "Object", &object);
    if (rc != URBI_OK) return rc;
    return urbi_slot_set(vm, object, name, ev);
}
static void report_heap(UVM *vm, const char *label)
{
    urbi_gc_collect(vm);
    UGcStats st; memset(&st, 0, sizeof st); (void)urbi_gc_stats(vm, &st);
    char line[160];
    int n = snprintf(line, sizeof line, "%s: alloc live %lu B, gc live %lu B, heap top %lu B, budget %lu B, cycles %lu\r\n",
                     label, (unsigned long)port_alloc_live_bytes(), (unsigned long)st.bytes_live,
                     (unsigned long)port_alloc_heap_top(), (unsigned long)st.heap_budget, (unsigned long)st.cycles);
    if (n > 0 && (size_t)n < sizeof line) port_console_write(line, (size_t)n);
}
static void fail(const char *what, UVM *vm)
{
    port_console_puts(what); port_console_puts(" FAILED\r\n");
    if (vm) { UErrorInfo info; memset(&info, 0, sizeof info); (void)urbi_last_error(vm, &info); port_console_puts("  "); port_console_puts(info.message ? info.message : "(no message)"); port_console_puts("\r\n"); }
    vTaskDelay(pdMS_TO_TICKS(200)); esp_restart();
}

static void urbi_task(void *arg)
{
    (void)arg;
    port_console_puts("\r\n\r\n=== urbi "); port_console_puts(urbi_version()); port_console_puts(" on ESP32-S3-EYE ===\r\n");
    char line[96];
    int n = snprintf(line, sizeof line, "arena %lu B, free internal heap %lu B\r\n", (unsigned long)ARENA_BYTES, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (n > 0 && (size_t)n < sizeof line) port_console_write(line, (size_t)n);

    UVMConfig cfg; memset(&cfg, 0, sizeof cfg);
    cfg.step_budget = STEP_BUDGET; cfg.boot_stdlib = 1; cfg.heap_budget = ARENA_BYTES / 4u * 3u;
    port_console_puts("[1] urbi_open... ");
    UVM *vm = urbi_open(port_alloc, NULL, &cfg);
    if (!vm || !urbi_realm_main(vm)) fail("urbi_open", NULL);
    urbi_set_clock(vm, port_time_us, NULL); urbi_set_writer(vm, port_writer, NULL); urbi_set_diag(vm, port_diag, NULL);
    port_console_puts("ok\r\n");
    report_heap(vm, "boot heap");

    port_console_puts("[2] fixtures... ");
    urbi_event_id_t blob, button, stats;
    if (urbi_register(vm, "Lobby.draw_crosshair", c_draw_crosshair, 2, 2) != URBI_OK ||
        urbi_register(vm, "Lobby.set_target_chroma", c_set_target_chroma, 3, 3) != URBI_OK ||
        urbi_register(vm, "Lobby.log", c_log, 1, 1) != URBI_OK ||
        urbi_register(vm, "Lobby.heap_live", c_heap_live, 0, 0) != URBI_OK ||
        urbi_register(vm, "Lobby.events_dropped", c_events_dropped, 0, 0) != URBI_OK ||
        bind_event(vm, "blob_seen", &blob) != URBI_OK || bind_event(vm, "button_pressed", &button) != URBI_OK ||
        bind_event(vm, "stats_tick", &stats) != URBI_OK) fail("fixtures", vm);
    eye_camera_bind(blob); eye_button_bind(button);
    port_console_puts("ok\r\n");

    port_console_puts("[3] workload... ");
    { UValue out = urbi_make_nil(); if (urbi_load(vm, urbi_realm_main(vm), eye_demo_wire, eye_demo_wire_len, &out) != URBI_OK) fail("workload", vm); }
    port_console_puts("ok\r\n");

    port_console_puts("[4] eval service... ");
    UReplConfig rcfg; memset(&rcfg, 0, sizeof rcfg);
    rcfg.output_buf_cap = USJ_TX_BYTES; rcfg.default_budget.max_parser_depth = BUDGET_DEPTH; rcfg.default_budget.max_ast_nodes = BUDGET_NODES; rcfg.default_budget.max_source_bytes = BUDGET_SOURCE;
    UReplServer *srv = NULL;
    if (urbi_repl_serve_init(vm, &rcfg, &srv) != URBI_OK) fail("eval service", vm);
    { UTransport t; port_usb_serial_jtag_vtable(&t); if (urbi_repl_register_transport(srv, &t) != URBI_OK) fail("transport", vm); }
    port_console_puts("ok\r\n");

    port_console_puts("[5] peripherals... ");
    eye_display_init(); eye_camera_init(); eye_button_init();
    port_console_puts("ok\r\n");
    report_heap(vm, "ready");
    port_console_puts("Session on this port.  One JSON request per line, e.g. {\"id\":1,\"op\":\"eval\",\"code\":\"1+1\"}\r\n");

    for (;;) {
        (void)port_queue_drain(vm);
        urbi_repl_serve_step(srv, 0);
        uint64_t wake_us = 0;
        int st = urbi_step(vm, STEP_BUDGET, &wake_us);
        if (st < 0) fail("urbi_step", vm);
        urbi_repl_serve_step(srv, 0);
        uint32_t wait_ms = LOOP_CAP_MS;
        if (st == URBI_STEP_RAN) wait_ms = 0;
        else if (st == URBI_STEP_IDLE_UNTIL) { uint64_t now = port_time_us(NULL); uint64_t until = wake_us > now ? (wake_us - now) / 1000u : 0u; if (until < wait_ms) wait_ms = (uint32_t)until; }
        (void)port_queue_wait(wait_ms);
    }
}

void app_main(void)
{
    port_console_init(port_usb_serial_jtag_raw_write);
    if (port_usb_serial_jtag_init(USJ_RX_BYTES, USJ_TX_BYTES, USJ_DEADLINE_MS) != 0) { ESP_LOGE("eye", "usb serial jtag init failed"); esp_restart(); }
    /* The IDF console is on this same port.  Until it is told to go
     * through the installed driver, stdout writes the FIFO directly and
     * races the driver's TX ring. */
    usb_serial_jtag_vfs_use_driver();
    esp_log_set_vprintf(port_console_vprintf);
    if (port_queue_init(QUEUE_DEPTH) != 0) esp_restart();
    xTaskCreatePinnedToCore(urbi_task, "urbi", URBI_TASK_STACK, NULL, tskIDLE_PRIORITY + 1, NULL, 0);
}
