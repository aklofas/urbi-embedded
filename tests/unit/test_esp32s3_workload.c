/* SPDX-License-Identifier: BSD-3-Clause */
/* The eye demo's workload on the host: a blob payload moves the crosshair
 * and counts, two presses inside the debounce window cycle the target once,
 * an off-panel blob is clamped by the board's own clamp, boot.stop() from
 * a session leaves later blobs unobserved, a session request past the
 * compile budget is refused and the session survives, and ten blobs
 * return to the same live bytes. */
#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/repl.h"
#include "repl/urepl_buffer_transport.h"
#include "../../examples/esp32/eye_demo/main/eye_clamp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { size_t n; double align; } Hdr;
static size_t s_live;
static void *counting_alloc(void *ptr, size_t n, void *ud)
{
    (void)ud;
    Hdr *h = ptr ? ((Hdr *)ptr - 1) : NULL;
    if (n == 0) { if (h) { s_live -= h->n; free(h); } return NULL; }
    size_t old = h ? h->n : 0;
    Hdr *nh = (Hdr *)realloc(h, sizeof(Hdr) + n);
    if (!nh) return NULL;
    nh->n = n; s_live += n - old;
    return nh + 1;
}
static uint64_t s_now_us;
static uint64_t fake_clock(void *ud) { (void)ud; return s_now_us; }

static long s_crosshairs, s_targets; static int s_cx, s_cy, s_dominant;
static char s_last_log[256];
static int c_draw_crosshair(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{ (void)vm; (void)self; (void)nargs; s_crosshairs++; s_cx = eye_clamp240((int)urbi_value_as_int(args[0])); s_cy = eye_clamp240((int)urbi_value_as_int(args[1])); *out = urbi_make_nil(); return UEXEC_OK; }
static int c_set_target_chroma(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{ (void)vm; (void)self; (void)nargs; s_targets++; s_dominant = (int)urbi_value_as_int(args[0]); *out = urbi_make_nil(); return UEXEC_OK; }
static int c_log(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{ (void)vm; (void)self; (void)nargs; const char *s = urbi_value_as_str(args[0], NULL); snprintf(s_last_log, sizeof s_last_log, "%s", s ? s : ""); *out = urbi_make_nil(); return UEXEC_OK; }
static int c_heap_live(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{ (void)vm; (void)self; (void)args; (void)nargs; *out = urbi_make_int((int64_t)s_live); return UEXEC_OK; }
static int c_events_dropped(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{ (void)vm; (void)self; (void)args; (void)nargs; *out = urbi_make_int(0); return UEXEC_OK; }

typedef struct { UVM *vm; urbi_event_id_t blob, button, stats; } Board;

static int bind_event(UVM *vm, const char *name, urbi_event_id_t *id)
{
    int rc = urbi_event_register(vm, NULL, name, id);
    if (rc != URBI_OK) return rc;
    UValue ev = urbi_make_nil(), object = urbi_make_nil();
    if ((rc = urbi_event_value(vm, *id, &ev)) != URBI_OK) return rc;
    if ((rc = urbi_global_get(vm, NULL, "Object", &object)) != URBI_OK) return rc;
    return urbi_slot_set(vm, object, name, ev);
}
static void board_open(Board *b)
{
    s_live = 0; s_crosshairs = s_targets = 0; s_cx = s_cy = -1; s_now_us = 1000000; s_last_log[0] = 0;
    UVMConfig cfg; memset(&cfg, 0, sizeof cfg);
    cfg.step_budget = 256; cfg.boot_stdlib = 1; cfg.heap_budget = 120u * 1024u;
    b->vm = urbi_open(counting_alloc, NULL, &cfg);
    UASSERT(b->vm != NULL);
    urbi_set_clock(b->vm, fake_clock, NULL);
    UASSERT_EQ(urbi_register(b->vm, "Lobby.draw_crosshair", c_draw_crosshair, 2, 2), URBI_OK);
    UASSERT_EQ(urbi_register(b->vm, "Lobby.set_target_chroma", c_set_target_chroma, 3, 3), URBI_OK);
    UASSERT_EQ(urbi_register(b->vm, "Lobby.log", c_log, 1, 1), URBI_OK);
    UASSERT_EQ(urbi_register(b->vm, "Lobby.heap_live", c_heap_live, 0, 0), URBI_OK);
    UASSERT_EQ(urbi_register(b->vm, "Lobby.events_dropped", c_events_dropped, 0, 0), URBI_OK);
    UASSERT_EQ(bind_event(b->vm, "blob_seen", &b->blob), URBI_OK);
    UASSERT_EQ(bind_event(b->vm, "button_pressed", &b->button), URBI_OK);
    UASSERT_EQ(bind_event(b->vm, "stats_tick", &b->stats), URBI_OK);
}
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb"); UASSERT(f != NULL); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *src = (char *)malloc((size_t)n + 1); size_t got = fread(src, 1, (size_t)n, f); fclose(f); src[got] = 0;
    return src;
}
static void board_load(Board *b)
{
    char *src = read_file("examples/esp32/eye_demo/main/eye_demo.u");
    uint8_t *bytes = NULL; size_t n = 0; char err[256] = {0};
    UASSERT_EQ(urbi_compile(b->vm, src, strlen(src), "eye_demo.u", &bytes, &n, err, sizeof err), URBI_OK);
    UValue out = urbi_make_nil();
    UASSERT_EQ(urbi_load(b->vm, urbi_realm_main(b->vm), bytes, n, &out), URBI_OK);
    urbi_chunk_free(b->vm, bytes, n); free(src);
}
static void run_quiet(Board *b)
{
    for (int i = 0; i < 100000; i++) { int st = urbi_step(b->vm, 256, NULL); if (st != URBI_STEP_RAN) return; }
    UASSERT(0 && "the workload never went quiet");
}
static void inject_blob(Board *b, int x, int y, uint32_t area)
{
    urbi_event_payload_t p; memset(&p, 0, sizeof p);
    p.u64[0] = (uint64_t)(uint16_t)x | ((uint64_t)(uint16_t)y << 16) | ((uint64_t)area << 32);
    UASSERT_EQ(urbi_inject_event(b->vm, b->blob, &p, 8), URBI_OK);
    run_quiet(b);
}
static int64_t int_global(Board *b, const char *path)
{
    UValue tracker = urbi_make_nil(), v = urbi_make_nil();
    UASSERT_EQ(urbi_global_get(b->vm, NULL, "Tracker", &tracker), URBI_OK);
    UASSERT_EQ(urbi_slot_get(b->vm, tracker, path, &v), URBI_OK);
    return urbi_value_as_int(v);
}

static void a_blob_moves_the_crosshair_and_counts(void)
{
    Board b; board_open(&b); board_load(&b); run_quiet(&b);
    UASSERT_EQ(s_targets, 1);                       /* Tracker.apply() at load */
    inject_blob(&b, 37, 201, 400);
    UASSERT_EQ(s_crosshairs, 1); UASSERT_EQ(s_cx, 37); UASSERT_EQ(s_cy, 201);
    UASSERT_EQ(int_global(&b, "count"), 1);
    inject_blob(&b, 38, 202, 401);
    UASSERT_EQ(int_global(&b, "count"), 2);
    s_now_us += 2000000; run_quiet(&b);           /* the 2 s stats line */
    UASSERT(strncmp(s_last_log, "stats: count 2, target red, live ", 33) == 0);
    UASSERT(strstr(s_last_log, " B, dropped 0") != NULL);
    urbi_close(b.vm);
}
static void an_off_panel_blob_is_clamped(void)
{
    Board b; board_open(&b); board_load(&b); run_quiet(&b);
    inject_blob(&b, 300, 65535, 1);          /* both off the panel; 65535 is the largest y the packing carries */
    UASSERT_EQ(s_cx, 239); UASSERT_EQ(s_cy, 239);
    UASSERT_EQ(int_global(&b, "x"), 300);    /* the script keeps the unclamped values */
    UASSERT_EQ(int_global(&b, "y"), 65535);
    urbi_close(b.vm);
}
static void two_presses_inside_200ms_cycle_once(void)
{
    Board b; board_open(&b); board_load(&b); run_quiet(&b);
    long before = s_targets;
    UASSERT_EQ(urbi_inject_event(b.vm, b.button, NULL, 0), URBI_OK); run_quiet(&b);
    s_now_us += 50000;
    UASSERT_EQ(urbi_inject_event(b.vm, b.button, NULL, 0), URBI_OK); run_quiet(&b);
    UASSERT_EQ(s_targets, before + 1);
    UASSERT_EQ(int_global(&b, "target"), 1);
    s_now_us += 300000;
    UASSERT_EQ(urbi_inject_event(b.vm, b.button, NULL, 0), URBI_OK); run_quiet(&b);
    UASSERT_EQ(s_targets, before + 2);
    UASSERT_EQ(int_global(&b, "target"), 2);
    UASSERT(strstr(s_last_log, "target: blue") != NULL);
    urbi_close(b.vm);
}
static void a_session_can_stop_boot_and_later_blobs_are_unobserved(void)
{
    Board b; board_open(&b); board_load(&b); run_quiet(&b);
    UReplConfig rc; memset(&rc, 0, sizeof rc); rc.output_buf_cap = 4096;
    rc.default_budget.max_parser_depth = 32; rc.default_budget.max_ast_nodes = 10000; rc.default_budget.max_source_bytes = 8192;
    UReplServer *srv = NULL; UASSERT_EQ(urbi_repl_serve_init(b.vm, &rc, &srv), URBI_OK);
    UBufferTransport *bt = urepl_buffer_transport_create(); UASSERT(bt != NULL);
    UTransport t; urepl_buffer_transport_vtable(bt, &t);
    UASSERT_EQ(urbi_repl_register_transport(srv, &t), URBI_OK);
    const char *req = "{\"id\":1,\"op\":\"eval\",\"code\":\"boot.stop()\"}\n";
    urepl_buffer_client_write(bt, req, strlen(req));
    urbi_repl_serve_step(srv, 0); run_quiet(&b); urbi_repl_serve_step(srv, 0);
    char outbuf[1024]; size_t n = urepl_buffer_client_read(bt, outbuf, sizeof outbuf - 1); outbuf[n] = 0;
    UASSERT(strstr(outbuf, "\"kind\":\"result\"") != NULL);
    long before = s_crosshairs;
    inject_blob(&b, 10, 10, 5);
    UASSERT_EQ(s_crosshairs, before);              /* nobody is watching */
    urbi_repl_serve_shutdown(srv); urepl_buffer_transport_destroy(bt);
    urbi_close(b.vm);
}
static void a_too_deep_session_request_is_refused_and_the_session_survives(void)
{
    Board b; board_open(&b); board_load(&b); run_quiet(&b);
    UReplConfig rc; memset(&rc, 0, sizeof rc); rc.output_buf_cap = 4096;
    rc.default_budget.max_parser_depth = 32; rc.default_budget.max_ast_nodes = 10000; rc.default_budget.max_source_bytes = 8192;
    UReplServer *srv = NULL; UASSERT_EQ(urbi_repl_serve_init(b.vm, &rc, &srv), URBI_OK);
    UBufferTransport *bt = urepl_buffer_transport_create(); UTransport t; urepl_buffer_transport_vtable(bt, &t);
    UASSERT_EQ(urbi_repl_register_transport(srv, &t), URBI_OK);
    char deep[512]; size_t k = 0; k += (size_t)snprintf(deep + k, sizeof deep - k, "{\"id\":1,\"op\":\"eval\",\"code\":\"");
    for (int i = 0; i < 40; i++) deep[k++] = '(';
    deep[k++] = '1'; for (int i = 0; i < 40; i++) deep[k++] = ')';
    k += (size_t)snprintf(deep + k, sizeof deep - k, "\"}\n");
    urepl_buffer_client_write(bt, deep, k);
    urbi_repl_serve_step(srv, 0); run_quiet(&b); urbi_repl_serve_step(srv, 0);
    char outbuf[2048]; size_t n = urepl_buffer_client_read(bt, outbuf, sizeof outbuf - 1); outbuf[n] = 0;
    UASSERT(strstr(outbuf, "\"kind\":\"error\"") != NULL);
    const char *req = "{\"id\":2,\"op\":\"eval\",\"code\":\"1+1\"}\n";
    urepl_buffer_client_write(bt, req, strlen(req));
    urbi_repl_serve_step(srv, 0); run_quiet(&b); urbi_repl_serve_step(srv, 0);
    n = urepl_buffer_client_read(bt, outbuf, sizeof outbuf - 1); outbuf[n] = 0;
    UASSERT(strstr(outbuf, "\"value\":\"2\"") != NULL);
    urbi_repl_serve_shutdown(srv); urepl_buffer_transport_destroy(bt);
    urbi_close(b.vm);
}
static void ten_blobs_return_to_the_same_live_bytes(void)
{
    Board b; board_open(&b); board_load(&b); run_quiet(&b);
    inject_blob(&b, 1, 1, 1); urbi_gc_collect(b.vm); size_t base = s_live;
    for (int i = 0; i < 10; i++) inject_blob(&b, i, i, (uint32_t)i);
    urbi_gc_collect(b.vm);
    UASSERT(s_live <= base + 1024 && base <= s_live + 1024);
    urbi_close(b.vm);
}
void test_esp32s3_workload_suite(void)
{
    utest_run("a_blob_moves_the_crosshair_and_counts", a_blob_moves_the_crosshair_and_counts);
    utest_run("an_off_panel_blob_is_clamped", an_off_panel_blob_is_clamped);
    utest_run("two_presses_inside_200ms_cycle_once", two_presses_inside_200ms_cycle_once);
    utest_run("a_session_can_stop_boot_and_later_blobs_are_unobserved", a_session_can_stop_boot_and_later_blobs_are_unobserved);
    utest_run("a_too_deep_session_request_is_refused_and_the_session_survives", a_too_deep_session_request_is_refused_and_the_session_survives);
    utest_run("ten_blobs_return_to_the_same_live_bytes", ten_blobs_return_to_the_same_live_bytes);
}
