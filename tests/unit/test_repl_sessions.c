/* SPDX-License-Identifier: BSD-3-Clause */
/* What a board's eval service relies on, run on the host: the shim's
 * configuration boots a standard library, host fixtures on Lobby and an
 * event on Object are visible unqualified from a session's own realm, the
 * Pico boot workload behaves, a session reopened after a close costs what
 * the first one did, a close in the middle of a periodic leaves nothing
 * behind, and the two per-session caps refuse without ending the
 * session.  Everything goes through <urbi/urbi.h>, <urbi/repl.h> and the
 * in-process buffer transport. */
#include "utest.h"
#include "urbi/urbi.h"
#include "urbi/repl.h"
#include "repl/urepl_buffer_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- a counting allocator, as the footprint probes count ------------- */
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
    nh->n = n;
    s_live += n - old;
    return nh + 1;
}

/* --- the board's stand-ins ------------------------------------------- */
static int s_toggles;
static double s_temp = 20.0;
static uint64_t s_now_us;
static char s_main_out[1024];

static int c_led_toggle(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    s_toggles++;
    *out = urbi_make_nil();
    return UEXEC_OK;
}
static int c_temp_celsius(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    *out = urbi_make_float(s_temp);
    return UEXEC_OK;
}
static uint64_t fake_clock(void *ud) { (void)ud; return s_now_us; }
static void main_writer(void *ud, const char *chan, size_t chan_len, const char *msg, size_t msg_len)
{
    (void)ud; (void)chan; (void)chan_len;
    size_t have = strlen(s_main_out);
    if (have + msg_len + 1 < sizeof s_main_out) { memcpy(s_main_out + have, msg, msg_len); s_main_out[have + msg_len] = '\0'; }
}

/* The shim's configuration, as examples/pico/repl_demo/main/main.c fills
 * it: the same step budget, the standard library on, and a heap budget
 * that on the board comes from the linker's heap minus a session reserve
 * (180 KB stands in for it here).  The compile budget numbers are
 * explained in that file. */
static UVMConfig shim_vm_config(void)
{
    UVMConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.step_budget = 256;
    cfg.boot_stdlib = 1;
    cfg.heap_budget = 180u * 1024u;
    return cfg;
}
static UReplConfig shim_repl_config(void)
{
    UReplConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.output_buf_cap = 4096;
    cfg.default_budget.max_parser_depth = 12;
    cfg.default_budget.max_ast_nodes    = 1000;
    cfg.default_budget.max_source_bytes = 1024;
    return cfg;
}

/* A VM with the board's fixtures installed the way the board installs
 * them: verbs on Lobby, the pressed event on Object. */
typedef struct { UVM *vm; UReplServer *srv; urbi_event_id_t pressed; } Board;

static void board_open(Board *b)
{
    s_live = 0; s_toggles = 0; s_temp = 20.0; s_now_us = 0; s_main_out[0] = '\0';
    UVMConfig cfg = shim_vm_config();
    b->vm = urbi_open(counting_alloc, NULL, &cfg);
    UASSERT(b->vm != NULL);
    urbi_set_clock(b->vm, fake_clock, NULL);
    urbi_set_writer(b->vm, main_writer, NULL);
    UASSERT_EQ(urbi_register(b->vm, "Lobby.led_toggle", c_led_toggle, 0, 0), URBI_OK);
    UASSERT_EQ(urbi_register(b->vm, "Lobby.temp_celsius", c_temp_celsius, 0, 0), URBI_OK);
    b->pressed = URBI_EVENT_ID_INVALID;
    UASSERT_EQ(urbi_event_register(b->vm, NULL, "pressed", &b->pressed), URBI_OK);
    UValue ev = urbi_make_nil(), object = urbi_make_nil();
    UASSERT_EQ(urbi_event_value(b->vm, b->pressed, &ev), URBI_OK);
    UASSERT_EQ(urbi_global_get(b->vm, NULL, "Object", &object), URBI_OK);
    UASSERT_EQ(urbi_slot_set(b->vm, object, "pressed", ev), URBI_OK);
    UReplConfig rc = shim_repl_config();
    UASSERT_EQ(urbi_repl_serve_init(b->vm, &rc, &b->srv), URBI_OK);
}
static void board_close(Board *b)
{
    urbi_repl_serve_shutdown(b->srv);
    urbi_close(b->vm);
    UASSERT_EQ(s_live, (size_t)0);
}
static void board_press(Board *b)
{
    UASSERT_EQ(urbi_inject_event(b->vm, b->pressed, NULL, 0), URBI_OK);
    (void)urbi_step(b->vm, 256, NULL);
}
static void board_advance(Board *b, uint64_t us)
{
    s_now_us += us;
    (void)urbi_step(b->vm, 256, NULL);
}
/* Feed one request line and sweep until the service is quiet; `out`
 * collects everything the client read. */
static void eval_line(Board *b, UBufferTransport *bt, const char *line, char *out, size_t cap)
{
    urepl_buffer_client_write(bt, line, strlen(line));
    size_t total = 0;
    for (int i = 0; i < 8; i++) {
        urbi_repl_serve_step(b->srv, 0);
        (void)urbi_step(b->vm, 256, NULL);
        urbi_repl_serve_step(b->srv, 0);
        total += urepl_buffer_client_read(bt, out + total, cap - 1 - total);
    }
    out[total] = '\0';
}
static UBufferTransport *session_open(Board *b)
{
    UBufferTransport *bt = urepl_buffer_transport_create();
    UTransport t;
    urepl_buffer_transport_vtable(bt, &t);
    UASSERT_EQ(urbi_repl_register_transport(b->srv, &t), URBI_OK);
    return bt;
}
static void session_close(Board *b, UBufferTransport *bt)
{
    urepl_buffer_client_finish(bt);
    for (int i = 0; i < 4; i++) { urbi_repl_serve_step(b->srv, 0); (void)urbi_step(b->vm, 256, NULL); }
    urepl_buffer_transport_destroy(bt);
}
static size_t settled_live(Board *b) { urbi_gc_collect(b->vm); return s_live; }

static char *slurp_workload(void)
{
    FILE *f = fopen("examples/pico/repl_demo/repl_demo.u", "rb");
    UASSERT(f != NULL);
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *src = (char *)malloc((size_t)n + 1);
    size_t got = fread(src, 1, (size_t)n, f);
    fclose(f);
    src[got] = '\0';
    return src;
}
static void run_workload(Board *b)
{
    char *src = slurp_workload();
    UValue out = urbi_make_nil();
    char err[256] = {0};
    int rc = urbi_run(b->vm, urbi_realm_main(b->vm), src, strlen(src), "repl_demo.u", &out, err, sizeof err);
    if (rc != URBI_OK) printf("    workload: rc=%d %s\n", rc, err);
    UASSERT_EQ(rc, URBI_OK);
    free(src);
}

/* --- the cases ---------------------------------------------------------- */

static void the_shim_config_boots_a_standard_library(void)
{
    Board b; board_open(&b);
    UValue out = urbi_make_nil();
    char err[256] = {0};
    const char *src = "echo(\"hi\") | Tag.new(\"t\") | 1 + 1";
    UASSERT_EQ(urbi_run(b.vm, urbi_realm_main(b.vm), src, strlen(src), NULL, &out, err, sizeof err), URBI_OK);
    UASSERT_EQ(urbi_value_as_int(out), 2);
    UASSERT(strstr(s_main_out, "hi") != NULL);
    UGcStats st;
    UASSERT_EQ(urbi_gc_stats(b.vm, &st), URBI_OK);
    UASSERT_EQ(st.heap_budget, (size_t)(180u * 1024u));
    board_close(&b);
}

static void a_lobby_fixture_and_an_object_event_are_visible_from_a_session(void)
{
    Board b; board_open(&b);
    UBufferTransport *bt = session_open(&b);
    char out[4096];
    eval_line(&b, bt, "{\"id\":1,\"op\":\"eval\",\"code\":\"led_toggle()\"}\n", out, sizeof out);
    UASSERT(strstr(out, "\"value\":\"nil\"") != NULL);
    UASSERT_EQ(s_toggles, 1);
    eval_line(&b, bt, "{\"id\":2,\"op\":\"eval\",\"code\":\"Lobby.temp_celsius()\"}\n", out, sizeof out);
    UASSERT(strstr(out, "\"value\":\"20.0\"") != NULL);
    eval_line(&b, bt, "{\"id\":3,\"op\":\"eval\",\"code\":\"at (pressed?) led_toggle()\"}\n", out, sizeof out);
    UASSERT(strstr(out, "\"kind\":\"error\"") == NULL);
    board_press(&b);
    UASSERT_EQ(s_toggles, 2);
    session_close(&b, bt);
    board_press(&b);                 /* the session's watcher died with its realm */
    UASSERT_EQ(s_toggles, 2);
    board_close(&b);
}

static void the_boot_workload_runs_and_a_session_can_stop_it(void)
{
    Board b; board_open(&b);
    run_workload(&b);
    board_press(&b);
    UASSERT_EQ(s_toggles, 1);
    s_temp = 50.5;
    board_advance(&b, 1100000);
    UASSERT(strstr(s_main_out, "warm") != NULL);
    s_main_out[0] = '\0';

    UBufferTransport *bt = session_open(&b);
    char out[4096];
    eval_line(&b, bt, "{\"id\":1,\"op\":\"eval\",\"code\":\"boot.stop()\"}\n", out, sizeof out);
    UASSERT(strstr(out, "\"kind\":\"error\"") == NULL);
    board_press(&b);
    UASSERT_EQ(s_toggles, 1);        /* the boot watcher is gone */
    board_advance(&b, 1100000);
    UASSERT(strstr(s_main_out, "warm") == NULL);   /* and so is the periodic */
    eval_line(&b, bt, "{\"id\":2,\"op\":\"eval\",\"code\":\"at (pressed?) led_toggle()\"}\n", out, sizeof out);
    board_press(&b);
    UASSERT_EQ(s_toggles, 2);        /* the session's own watcher took over */
    session_close(&b, bt);
    board_close(&b);
}

static void a_session_reopened_after_close_costs_within_a_kilobyte(void)
{
    Board b; board_open(&b);
    run_workload(&b);
    size_t before = settled_live(&b);
    size_t after_first = 0, after_second = 0;
    for (int round = 0; round < 2; round++) {
        UBufferTransport *bt = session_open(&b);
        char out[4096];
        eval_line(&b, bt, "{\"id\":1,\"op\":\"eval\",\"code\":\"echo(\\\"hi\\\")\"}\n", out, sizeof out);
        eval_line(&b, bt, "{\"id\":2,\"op\":\"eval\",\"code\":\"1+1\"}\n", out, sizeof out);
        UASSERT(strstr(out, "\"value\":\"2\"") != NULL);
        eval_line(&b, bt, "{\"id\":3,\"op\":\"eval\",\"code\":\"var t = Tag.new(\\\"t\\\") | t: every (1s) echo(temp_celsius())\"}\n", out, sizeof out);
        board_advance(&b, 2500000);
        eval_line(&b, bt, "{\"id\":4,\"op\":\"eval\",\"code\":\"t.stop()\"}\n", out, sizeof out);
        session_close(&b, bt);
        size_t now = settled_live(&b);
        if (round == 0) after_first = now; else after_second = now;
    }
    printf("    session reopen: boot %lu, after first close %lu, after second %lu\n",
           (unsigned long)before, (unsigned long)after_first, (unsigned long)after_second);
    UASSERT(after_first >= before);
    UASSERT(after_first - before < 1024);
    UASSERT(after_second <= after_first + 256);
    board_close(&b);
}

static void a_session_closed_with_a_periodic_running_leaves_nothing_behind(void)
{
    Board b; board_open(&b);
    size_t before = settled_live(&b);
    UBufferTransport *bt = session_open(&b);
    char out[4096];
    eval_line(&b, bt, "{\"id\":1,\"op\":\"eval\",\"code\":\"var t = Tag.new(\\\"t\\\") | t: every (1s) led_toggle()\"}\n", out, sizeof out);
    board_advance(&b, 1100000);
    int fired = s_toggles;
    UASSERT(fired >= 1);
    session_close(&b, bt);           /* DTR dropped with the periodic armed */
    board_advance(&b, 5000000);
    UASSERT_EQ(s_toggles, fired);    /* the realm's timer went with the realm */
    size_t after = settled_live(&b);
    UASSERT(after >= before);
    UASSERT(after - before < 1024);
    board_close(&b);
}

static void a_line_over_the_source_budget_is_refused_and_the_session_survives(void)
{
    Board b; board_open(&b);
    UBufferTransport *bt = session_open(&b);
    char line[6000];
    size_t at = (size_t)snprintf(line, sizeof line, "{\"id\":1,\"op\":\"eval\",\"code\":\"");
    while (at < 5200) { line[at++] = '1'; line[at++] = '+'; }
    at += (size_t)snprintf(line + at, sizeof line - at, "1\"}\n");
    char out[8192];
    eval_line(&b, bt, line, out, sizeof out);
    UASSERT(strstr(out, "\"code\":\"budget_source\"") != NULL);
    eval_line(&b, bt, "{\"id\":2,\"op\":\"eval\",\"code\":\"1+1\"}\n", out, sizeof out);
    UASSERT(strstr(out, "\"value\":\"2\"") != NULL);
    session_close(&b, bt);
    board_close(&b);
}

static void output_past_the_pico_cap_is_reported_dropped_and_the_session_continues(void)
{
    Board b; board_open(&b);
    UBufferTransport *bt = session_open(&b);
    /* A 700-character string literal, echoed eight times in one eval, so
     * the code stays under the 1 KiB source budget.  echo's own per-line
     * frame caps each write at roughly 1 KB before it reaches the
     * session's output buffer (lobby_send's local buffer), so each echo
     * stages about 720 bytes; eight of them stacked in one atomic eval
     * overrun the 4 KB cap. */
    char line[2000];
    size_t at = (size_t)snprintf(line, sizeof line, "{\"id\":1,\"op\":\"eval\",\"code\":\"var s = \\\"");
    for (int i = 0; i < 700; i++) line[at++] = 'a';
    at += (size_t)snprintf(line + at, sizeof line - at,
                           "\\\" | echo(s) | echo(s) | echo(s) | echo(s) | echo(s) | echo(s) | echo(s) | echo(s)\"}\n");
    char out[16384];
    eval_line(&b, bt, line, out, sizeof out);
    UASSERT(strstr(out, "\"code\":\"output_dropped\"") != NULL);
    eval_line(&b, bt, "{\"id\":2,\"op\":\"eval\",\"code\":\"1+1\"}\n", out, sizeof out);
    UASSERT(strstr(out, "\"value\":\"2\"") != NULL);
    session_close(&b, bt);
    board_close(&b);
}

void test_repl_sessions_suite(void)
{
    utest_run("the_shim_config_boots_a_standard_library", the_shim_config_boots_a_standard_library);
    utest_run("a_lobby_fixture_and_an_object_event_are_visible_from_a_session", a_lobby_fixture_and_an_object_event_are_visible_from_a_session);
    utest_run("the_boot_workload_runs_and_a_session_can_stop_it", the_boot_workload_runs_and_a_session_can_stop_it);
    utest_run("a_session_reopened_after_close_costs_within_a_kilobyte", a_session_reopened_after_close_costs_within_a_kilobyte);
    utest_run("a_session_closed_with_a_periodic_running_leaves_nothing_behind", a_session_closed_with_a_periodic_running_leaves_nothing_behind);
    utest_run("a_line_over_the_source_budget_is_refused_and_the_session_survives", a_line_over_the_source_budget_is_refused_and_the_session_survives);
    utest_run("output_past_the_pico_cap_is_reported_dropped_and_the_session_continues", output_past_the_pico_cap_is_reported_dropped_and_the_session_continues);
}
