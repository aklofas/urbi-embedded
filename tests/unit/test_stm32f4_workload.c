/* SPDX-License-Identifier: BSD-3-Clause */
/* The Mandelbrot workload, on the host, at a small canvas: it renders
 * through the lcd verb, returns from loading before the first render is
 * done, re-renders on the button event, and gives every byte back
 * between renders.  Everything through <urbi/urbi.h>; the board verbs
 * are stand-ins that count. */
#include "utest.h"
#include "urbi/urbi.h"
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

static long s_rects, s_begins, s_ends;
static uint64_t s_now_us;
static int c_fill_rect(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{ (void)vm; (void)self; (void)args; (void)nargs; s_rects++; *out = urbi_make_nil(); return UEXEC_OK; }
static int c_gyro(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{ (void)vm; (void)self; (void)args; (void)nargs; *out = urbi_make_float(0.0); return UEXEC_OK; }
static int c_render_begin(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{ (void)vm; (void)self; (void)args; (void)nargs; s_begins++; *out = urbi_make_nil(); return UEXEC_OK; }
static int c_render_end(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{ (void)vm; (void)self; (void)args; (void)nargs; s_ends++; *out = urbi_make_nil(); return UEXEC_OK; }
static uint64_t fake_clock(void *ud) { (void)ud; return s_now_us; }

/* The workload text with its canvas shrunk to 32 x 24 so a full render is
 * 6 levels of at most 768 cells, not 76,800. */
static char *small_workload(void)
{
    FILE *f = fopen("examples/stm32f4/mandelbrot/mandelbrot.u", "rb");
    UASSERT(f != NULL);
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *src = (char *)malloc((size_t)n + 1);
    size_t got = fread(src, 1, (size_t)n, f); fclose(f); src[got] = '\0';
    char *w = strstr(src, "Realm.W        = 320;");
    char *h = strstr(src, "Realm.H        = 240;");
    UASSERT(w != NULL && h != NULL);
    if (w) memcpy(w, "Realm.W        = 32; ", 21);
    if (h) memcpy(h, "Realm.H        = 24; ", 21);
    return src;
}

typedef struct { UVM *vm; urbi_event_id_t button, tick; } Board;

static void board_open(Board *b)
{
    s_live = 0; s_rects = s_begins = s_ends = 0; s_now_us = 0;
    UVMConfig cfg; memset(&cfg, 0, sizeof cfg);
    cfg.step_budget = 256; cfg.boot_stdlib = 1; cfg.heap_budget = 128u * 1024u;
    b->vm = urbi_open(counting_alloc, NULL, &cfg);
    UASSERT(b->vm != NULL);
    urbi_set_clock(b->vm, fake_clock, NULL);
    UASSERT_EQ(urbi_register(b->vm, "lcd_fill_rect", c_fill_rect, 5, 5), URBI_OK);
    UASSERT_EQ(urbi_register(b->vm, "gyro_x", c_gyro, 0, 0), URBI_OK);
    UASSERT_EQ(urbi_register(b->vm, "gyro_y", c_gyro, 0, 0), URBI_OK);
    UASSERT_EQ(urbi_register(b->vm, "gyro_z", c_gyro, 0, 0), URBI_OK);
    UASSERT_EQ(urbi_register(b->vm, "render_begin", c_render_begin, 0, 0), URBI_OK);
    UASSERT_EQ(urbi_register(b->vm, "render_end", c_render_end, 0, 0), URBI_OK);
    UValue ev = urbi_make_nil();
    UASSERT_EQ(urbi_event_register(b->vm, NULL, "button_press", &b->button), URBI_OK);
    UASSERT_EQ(urbi_event_value(b->vm, b->button, &ev), URBI_OK);
    UASSERT_EQ(urbi_global_set(b->vm, NULL, "button_press", ev), URBI_OK);
    UASSERT_EQ(urbi_event_register(b->vm, NULL, "gyro_tick", &b->tick), URBI_OK);
    UASSERT_EQ(urbi_event_value(b->vm, b->tick, &ev), URBI_OK);
    UASSERT_EQ(urbi_global_set(b->vm, NULL, "gyro_tick", ev), URBI_OK);
}
static void board_load(Board *b)
{
    char *src = small_workload();
    uint8_t *bytes = NULL; size_t n = 0; char err[256] = {0};
    UASSERT_EQ(urbi_compile(b->vm, src, strlen(src), "mandelbrot.u", &bytes, &n, err, sizeof err), URBI_OK);
    UValue out = urbi_make_nil();
    UASSERT_EQ(urbi_load(b->vm, urbi_realm_main(b->vm), bytes, n, &out), URBI_OK);
    urbi_chunk_free(b->vm, bytes, n);
    free(src);
}
static void board_run_until_quiet(Board *b)
{
    for (int i = 0; i < 100000; i++) {
        int st = urbi_step(b->vm, 256, NULL);
        if (st != URBI_STEP_RAN) return;
    }
    UASSERT(!"the render did not finish within 100,000 slices");
}
static size_t settled_live(Board *b) { urbi_gc_collect(b->vm); return s_live; }
static void board_close(Board *b) { urbi_close(b->vm); UASSERT_EQ(s_live, (size_t)0); }

/* Tiles in one full render of the 32 x 24 canvas: render_level steps x and
 * y by L from 0, so each level draws ceil(32/L) * ceil(24/L) tiles --
 * 1 + 4 + 12 + 48 + 192 + 768 = 1,025. */
static long full_render_rects(void)
{
    long expect = 0;
    int levels[6] = { 32, 16, 8, 4, 2, 1 };
    for (int i = 0; i < 6; i++) expect += ((32 + levels[i] - 1) / levels[i]) * ((24 + levels[i] - 1) / levels[i]);
    return expect;
}

static void the_workload_returns_from_load_before_the_first_render_finishes(void)
{
    Board b; board_open(&b); board_load(&b);
    UASSERT_EQ(s_ends, 0);              /* load returned with the render not done */
    board_run_until_quiet(&b);
    UASSERT_EQ(s_begins, 1);
    UASSERT_EQ(s_ends, 1);
    UASSERT_EQ(s_rects, full_render_rects());
    board_close(&b);
}

static void a_button_press_re_renders_at_half_the_span(void)
{
    Board b; board_open(&b); board_load(&b); board_run_until_quiet(&b);
    UValue span = urbi_make_nil();
    UASSERT_EQ(urbi_global_get(b.vm, NULL, "span", &span), URBI_OK);
    double before = urbi_value_as_float(span);
    UASSERT_EQ(urbi_inject_event(b.vm, b.button, NULL, 0), URBI_OK);
    board_run_until_quiet(&b);
    UASSERT_EQ(urbi_global_get(b.vm, NULL, "span", &span), URBI_OK);
    UASSERT(urbi_value_as_float(span) < before);
    UASSERT_EQ(s_ends, 2);
    board_close(&b);
}

static void ten_re_renders_return_to_the_same_live_bytes(void)
{
    Board b; board_open(&b); board_load(&b); board_run_until_quiet(&b);
    size_t after_first = settled_live(&b);
    size_t worst = 0;
    for (int i = 0; i < 10; i++) {
        UASSERT_EQ(urbi_inject_event(b.vm, b.button, NULL, 0), URBI_OK);
        board_run_until_quiet(&b);
        size_t now = settled_live(&b);
        size_t drift = now > after_first ? now - after_first : after_first - now;
        if (drift > worst) worst = drift;
    }
    printf("    re-render drift: worst %lu bytes over 10 renders\n", (unsigned long)worst);
    UASSERT(worst < 1024);
    UASSERT_EQ(s_ends, 11);
    board_close(&b);
}

static void a_press_during_a_render_aborts_and_restarts_it(void)
{
    Board b; board_open(&b); board_load(&b);
    /* A few slices in, the render is under way. */
    for (int i = 0; i < 20; i++) UASSERT_EQ(urbi_step(b.vm, 256, NULL), URBI_STEP_RAN);
    UASSERT_EQ(s_ends, 0);
    UASSERT_EQ(urbi_inject_event(b.vm, b.button, NULL, 0), URBI_OK);
    board_run_until_quiet(&b);
    /* The press sets redraw_requested, so render_all returns early and the
     * loop still calls render_end for the cut-short pass; it then finds the
     * flag set, skips the wait and renders again in full.  Two begins, two
     * ends, and fewer tiles than two full renders. */
    UASSERT_EQ(s_begins, 2);
    UASSERT_EQ(s_ends, 2);
    UASSERT(s_rects < 2 * full_render_rects());
    board_close(&b);
}

void test_stm32f4_workload_suite(void)
{
    utest_run("the_workload_returns_from_load_before_the_first_render_finishes", the_workload_returns_from_load_before_the_first_render_finishes);
    utest_run("a_button_press_re_renders_at_half_the_span", a_button_press_re_renders_at_half_the_span);
    utest_run("ten_re_renders_return_to_the_same_live_bytes", ten_re_renders_return_to_the_same_live_bytes);
    utest_run("a_press_during_a_render_aborts_and_restarts_it", a_press_during_a_render_aborts_and_restarts_it);
}
