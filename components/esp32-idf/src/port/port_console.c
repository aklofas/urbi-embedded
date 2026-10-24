/* SPDX-License-Identifier: BSD-3-Clause */
/* Console framing: every line the shim prints leaves as one NDJSON output
 * envelope on the "console" channel,
 *
 *   {"kind":"output","channel":"console","msg":"<escaped line>"}\n
 *
 * so a host reading the USB Serial/JTAG stream sees nothing but JSON.  A
 * line is framed whole in a buffer and offered to the sink once.  The
 * sink is all-or-nothing; if it refuses the line, or the line outgrew its
 * buffer, the line is dropped and counted, so the wire never carries half
 * a line followed by another envelope.
 *
 * Two paths, two kinds of buffer:
 *   - begin/part/end (and write/puts, which compose them) stage the line
 *     in one static buffer with no lock.  They are for the task that owns
 *     the VM: the writer, the diag hook and the shim's own prints.
 *   - vprintf is installed with esp_log_set_vprintf and runs on whatever
 *     task logs.  It formats and frames into buffers on its own stack and
 *     offers the envelope to the sink directly, never touching the staged
 *     line, so it is safe from any task, even while a staged line is open.
 * The dropped counter is the only state both paths share; it is bumped
 * atomically. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "port_esp_idf.h"

#define CONSOLE_LINE_BYTES  512U
#define CONSOLE_FMT_BYTES   256U

static const char s_head[] = "{\"kind\":\"output\",\"channel\":\"console\",\"msg\":\"";
static const char s_tail[] = "\"}\n";

static port_console_sink_fn s_sink;
static char     s_line[CONSOLE_LINE_BYTES];
static size_t   s_used;
static int      s_overflowed;
static uint32_t s_dropped;

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

static void count_drop(void)
{
    __atomic_add_fetch(&s_dropped, 1U, __ATOMIC_RELAXED);
}

/* Escapes s into line[*used..], leaving room for the tail so a line that
 * fits can always be closed.  Returns 0 once the line no longer fits. */
static int frame_part(char *line, size_t cap, size_t *used, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char esc[6];
        size_t k = json_escape_byte((unsigned char)s[i], esc);
        if (*used + k > cap - (sizeof s_tail - 1U)) return 0;
        memcpy(&line[*used], esc, k);
        *used += k;
    }
    return 1;
}

/* Closes the envelope and offers it to the sink once. */
static void frame_offer(char *line, size_t used)
{
    port_console_sink_fn sink = s_sink;
    if (sink == NULL) {
        count_drop();
        return;
    }
    memcpy(&line[used], s_tail, sizeof s_tail - 1U);
    size_t total = used + sizeof s_tail - 1U;
    int took = sink(line, total);
    if (took < 0 || (size_t)took != total) count_drop();
}

void port_console_init(port_console_sink_fn sink)
{
    s_sink = sink;
    s_used = 0;
    s_overflowed = 0;
}

void port_console_begin(void)
{
    memcpy(s_line, s_head, sizeof s_head - 1U);
    s_used = sizeof s_head - 1U;
    s_overflowed = 0;
}

void port_console_part(const char *s, size_t n)
{
    if (!s_overflowed && !frame_part(s_line, sizeof s_line, &s_used, s, n)) s_overflowed = 1;
}

void port_console_end(void)
{
    if (s_overflowed) {
        count_drop();
        return;
    }
    frame_offer(s_line, s_used);
}

void port_console_write(const char *s, size_t n)
{
    port_console_begin();
    port_console_part(s, n);
    port_console_end();
}

void port_console_puts(const char *s)
{
    port_console_write(s, strlen(s));
}

int port_console_vprintf(const char *fmt, va_list ap)
{
    char text[CONSOLE_FMT_BYTES];
    int n = vsnprintf(text, sizeof text, fmt, ap);
    if (n < 0) return n;
    size_t len = (size_t)n < sizeof text ? (size_t)n : sizeof text - 1U;

    char line[CONSOLE_LINE_BYTES];
    size_t used = sizeof s_head - 1U;
    memcpy(line, s_head, used);
    if (frame_part(line, sizeof line, &used, text, len)) frame_offer(line, used);
    else count_drop();
    return n;
}

uint32_t port_console_dropped_lines(void)
{
    return __atomic_load_n(&s_dropped, __ATOMIC_RELAXED);
}
