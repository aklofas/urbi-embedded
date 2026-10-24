/* SPDX-License-Identifier: BSD-3-Clause */
/* The qemu gate: the component's arena, a baked workload, three payload
 * injections through the queue, the eval service on an in-memory
 * transport, and the figures the board must match.  No peripheral. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "urbi/urbi.h"
#include "urbi/repl.h"
#include "port_esp_idf.h"
#include "smoke_transport.h"
#include "reactive_smoke_baked.h"

#define SMOKE_TASK_STACK (64u * 1024u)   /* generous: the point is to measure */
#define STEP_BUDGET      256u

static int stdout_sink(const void *buf, size_t n) { fwrite(buf, 1, n, stdout); fflush(stdout); return (int)n; }

static int c_pass(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{ (void)vm; (void)self; (void)nargs; printf("RESULT: PASS seq=%ld\n", (long)urbi_value_as_int(args[0])); *out = urbi_make_nil(); return UEXEC_OK; }

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
    printf("%s: alloc live %lu B, gc live %lu B, heap top %lu B, budget %lu B, cycles %lu\n",
           label, (unsigned long)port_alloc_live_bytes(), (unsigned long)st.bytes_live,
           (unsigned long)port_alloc_heap_top(), (unsigned long)st.heap_budget, (unsigned long)st.cycles);
}

static void smoke_task(void *arg)
{
    (void)arg;
    printf("=== URBI SMOKE START ===\n");
    UVMConfig cfg; memset(&cfg, 0, sizeof cfg);
    cfg.step_budget = STEP_BUDGET; cfg.boot_stdlib = 1; cfg.heap_budget = (size_t)CONFIG_URBI_ARENA_BYTES / 4u * 3u;
    UVM *vm = urbi_open(port_alloc, NULL, &cfg);
    if (!vm) { printf("FAIL: urbi_open\n"); goto done; }
    urbi_set_clock(vm, port_time_us, NULL); urbi_set_writer(vm, port_writer, NULL); urbi_set_diag(vm, port_diag, NULL);
    report_heap(vm, "boot heap");

    urbi_event_id_t ping;
    if (urbi_register(vm, "Lobby.pass", c_pass, 1, 1) != URBI_OK || bind_event(vm, "ping", &ping) != URBI_OK) { printf("FAIL: fixtures\n"); goto done; }
    { UValue out = urbi_make_nil(); if (urbi_load(vm, urbi_realm_main(vm), reactive_smoke_wire, reactive_smoke_wire_len, &out) != URBI_OK) { printf("FAIL: load\n"); goto done; } }
    if (port_queue_init(8) != 0) { printf("FAIL: queue\n"); goto done; }
    for (uint64_t seq = 1; seq <= 3; seq++) { port_event_rec r = { .id = ping, .payload = seq, .has_payload = 1 }; (void)port_queue_post(&r); }
    for (int i = 0; i < 1000; i++) { (void)port_queue_drain(vm); if (urbi_step(vm, STEP_BUDGET, NULL) != URBI_STEP_RAN) break; }

    UReplConfig rcfg; memset(&rcfg, 0, sizeof rcfg);
    rcfg.output_buf_cap = 4096; rcfg.default_budget.max_parser_depth = 32; rcfg.default_budget.max_ast_nodes = 10000; rcfg.default_budget.max_source_bytes = 8192;
    UReplServer *srv = NULL; UTransport t; smoke_transport_vtable(&t);
    if (urbi_repl_serve_init(vm, &rcfg, &srv) != URBI_OK || urbi_repl_register_transport(srv, &t) != URBI_OK) { printf("FAIL: session\n"); goto done; }
    /* A request near the parser-depth budget, so the high-water below covers a deep parse. */
    char req[256]; size_t k = (size_t)snprintf(req, sizeof req, "{\"id\":1,\"op\":\"eval\",\"code\":\"");
    for (int i = 0; i < 30; i++) req[k++] = '(';
    req[k++] = '1'; req[k++] = '+'; req[k++] = '1';
    for (int i = 0; i < 30; i++) req[k++] = ')';
    k += (size_t)snprintf(req + k, sizeof req - k, "\"}\n");
    smoke_transport_feed(req, k);
    for (int i = 0; i < 100; i++) { urbi_repl_serve_step(srv, 0); if (urbi_step(vm, STEP_BUDGET, NULL) != URBI_STEP_RAN) { urbi_repl_serve_step(srv, 0); break; } }
    char outbuf[512]; size_t n = smoke_transport_take(outbuf, sizeof outbuf - 1); outbuf[n] = 0;
    if (strstr(outbuf, "\"value\":\"2\"")) printf("SESSION: 2\n");
    else printf("SESSION: unexpected reply %s\n", outbuf);
    report_heap(vm, "session open");
    printf("STACK: high water %lu B free of %lu\n", (unsigned long)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)), (unsigned long)SMOKE_TASK_STACK);
done:
    printf("DONE\n"); fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();   /* -no-reboot makes qemu exit here */
}

void app_main(void)
{
    port_console_init(stdout_sink);
    xTaskCreatePinnedToCore(smoke_task, "smoke", SMOKE_TASK_STACK, NULL, tskIDLE_PRIORITY + 1, NULL, 0);
}
