/* SPDX-License-Identifier: BSD-3-Clause */
#include "../../components/esp32-idf/src/port/port_esp_idf.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static char   sink_buf[8192];
static size_t sink_len;
static size_t sink_cap_per_call;          /* 0 = accept everything */
/* Models the driver: all-or-nothing, refusing with 0 anything longer than
 * the cap. */
static int capture_sink(const void *buf, size_t n)
{
    if (sink_cap_per_call && n > sink_cap_per_call) return 0;
    assert(sink_len + n < sizeof sink_buf);
    memcpy(sink_buf + sink_len, buf, n); sink_len += n;
    return (int)n;
}
static void reset(size_t cap) { sink_len = 0; sink_cap_per_call = cap; memset(sink_buf, 0, sizeof sink_buf); port_console_init(capture_sink); }

/* One console line is one envelope, with JSON escaping, ending in a newline. */
static void test_a_line_is_one_envelope(void)
{
    reset(0);
    port_console_write("say \"hi\"\r\n", 10);
    assert(strcmp(sink_buf, "{\"kind\":\"output\",\"channel\":\"console\",\"msg\":\"say \\\"hi\\\"\\r\\n\"}\n") == 0);
    printf("test_a_line_is_one_envelope PASS\n");
}
/* A line written in parts is still one envelope. */
static void test_parts_make_one_envelope(void)
{
    reset(0);
    port_console_begin(); port_console_part("ab", 2); port_console_part("cd", 2); port_console_end();
    assert(strcmp(sink_buf, "{\"kind\":\"output\",\"channel\":\"console\",\"msg\":\"abcd\"}\n") == 0);
    assert(strstr(sink_buf, "\n") == sink_buf + strlen(sink_buf) - 1);   /* exactly one newline */
    printf("test_parts_make_one_envelope PASS\n");
}
/* A sink that cannot take a line whole does not get half a line followed
 * by another envelope: the line is dropped whole and counted, and nothing
 * reaches the wire. */
static void test_console_a_short_sink_drops_the_line_whole(void)
{
    reset(5);
    uint32_t before = port_console_dropped_lines();
    port_console_write("0123456789", 10);
    port_console_write("next", 4);
    assert(port_console_dropped_lines() == before + 2);
    assert(sink_len == 0);
    printf("test_console_a_short_sink_drops_the_line_whole PASS\n");
}
/* ESP_LOG's vprintf hook lands in the same framing. */
static int call_vprintf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int n = port_console_vprintf(fmt, ap); va_end(ap); return n; }
static void test_vprintf_is_framed(void)
{
    reset(0);
    int n = call_vprintf("I (%d) cam: %s\n", 12, "ok");
    assert(n > 0);
    assert(strcmp(sink_buf, "{\"kind\":\"output\",\"channel\":\"console\",\"msg\":\"I (12) cam: ok\\n\"}\n") == 0);
    printf("test_vprintf_is_framed PASS\n");
}
/* A log line framed through vprintf while a staged line is open does not
 * touch the staged line: both envelopes come out whole and separate. */
static void test_vprintf_leaves_an_open_staged_line_intact(void)
{
    reset(0);
    port_console_begin();
    port_console_part("ab", 2);
    int n = call_vprintf("log %d\n", 7);
    assert(n == 6);
    port_console_part("cd", 2);
    port_console_end();
    assert(strcmp(sink_buf,
                  "{\"kind\":\"output\",\"channel\":\"console\",\"msg\":\"log 7\\n\"}\n"
                  "{\"kind\":\"output\",\"channel\":\"console\",\"msg\":\"abcd\"}\n") == 0);
    printf("test_vprintf_leaves_an_open_staged_line_intact PASS\n");
}
int main(void)
{
    test_a_line_is_one_envelope();
    test_parts_make_one_envelope();
    test_console_a_short_sink_drops_the_line_whole();
    test_vprintf_is_framed();
    test_vprintf_leaves_an_open_staged_line_intact();
    return 0;
}
