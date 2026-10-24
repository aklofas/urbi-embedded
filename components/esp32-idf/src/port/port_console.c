/* SPDX-License-Identifier: BSD-3-Clause */
/* Console framing: every line the shim prints leaves as one NDJSON output
 * envelope on the "console" channel,
 *
 *   {"kind":"output","channel":"console","msg":"<escaped line>"}\n
 *
 * so a host reading the USB Serial/JTAG stream sees nothing but JSON.  A
 * line is staged whole in a static buffer and offered to the sink once.
 * If the sink takes less than all of it, or the line outgrew the buffer,
 * the line is dropped and counted: nothing further is offered, so the
 * wire never carries half a line followed by another envelope.
 *
 * Called from the task that owns the VM only (the writer, the diag hook
 * and the shim's own prints); the staging buffer has no lock. */
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
    for (size_t i = 0; i < n && !s_overflowed; i++) {
        char esc[6];
        size_t k = json_escape_byte((unsigned char)s[i], esc);
        /* Leave room for the tail so a line that fits is always closed. */
        if (s_used + k > sizeof s_line - (sizeof s_tail - 1U)) {
            s_overflowed = 1;
            break;
        }
        memcpy(&s_line[s_used], esc, k);
        s_used += k;
    }
}

void port_console_end(void)
{
    if (s_overflowed || s_sink == NULL) {
        s_dropped++;
        return;
    }
    memcpy(&s_line[s_used], s_tail, sizeof s_tail - 1U);
    size_t total = s_used + sizeof s_tail - 1U;
    int took = s_sink(s_line, total);
    if (took < 0 || (size_t)took != total) s_dropped++;
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
    char buf[CONSOLE_FMT_BYTES];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) return n;
    size_t len = (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1U;
    port_console_write(buf, len);
    return n;
}

uint32_t port_console_dropped_lines(void)
{
    return s_dropped;
}
