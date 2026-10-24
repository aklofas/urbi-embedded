/* SPDX-License-Identifier: BSD-3-Clause */
/* ESP-IDF port glue for urbi: the arena allocator, the clock, the
 * console framing, the writer and diag hooks, the event queue and the
 * USB Serial/JTAG transport.  The application includes this header and
 * hands the functions below to the runtime's hooks. */
#ifndef URBI_PORT_ESP_IDF_H
#define URBI_PORT_ESP_IDF_H
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include "urbi/types.h"
#include "urbi/repl.h"
#ifdef __cplusplus
extern "C" {
#endif

/* The arena.  CONFIG_URBI_ARENA_BYTES comes from the project's Kconfig
 * (examples/esp32/eye_demo/main/Kconfig.projbuild); the host tests define
 * URBI_ARENA_BYTES directly. */
#ifndef URBI_ARENA_BYTES
#  ifdef CONFIG_URBI_ARENA_BYTES
#    define URBI_ARENA_BYTES CONFIG_URBI_ARENA_BYTES
#  else
#    define URBI_ARENA_BYTES (160U * 1024U)
#  endif
#endif
void    *port_alloc(void *ptr, size_t nbytes, void *ud);      /* UVMAllocFn */
size_t   port_alloc_live_bytes(void);     /* requested bytes outstanding: the figure the 48 KB cap caps */
size_t   port_alloc_heap_top(void);       /* bump high-water mark, headers included */
size_t   port_alloc_heap_size(void);
size_t   port_alloc_count(void);
size_t   port_alloc_free_count(void);
size_t   port_alloc_freelist_hits(void);
size_t   port_alloc_last_failed_request(void);

uint64_t port_time_us(void *ud);                                /* clock hook: esp_timer_get_time */

/* Console framing.  Everything the shim prints goes out as one NDJSON
 * output envelope per line on the "console" channel, through the sink
 * the shim installs (the USB Serial/JTAG transport's raw write).  A line
 * is never split across two envelopes; a line the sink cannot take whole
 * within its deadline is dropped whole.
 *
 * The sink is all-or-nothing: it accepts the whole buffer and returns n,
 * or accepts none of it and returns 0.  begin/part/end, write and puts
 * share one static staging buffer and belong to the VM task.
 * port_console_vprintf frames on its own stack and is safe from any task. */
typedef int (*port_console_sink_fn)(const void *buf, size_t n);  /* n if accepted whole, else 0 */
void     port_console_init(port_console_sink_fn sink);
void     port_console_begin(void);
void     port_console_part(const char *s, size_t n);
void     port_console_end(void);
void     port_console_write(const char *s, size_t n);
void     port_console_puts(const char *s);
int      port_console_vprintf(const char *fmt, va_list ap);  /* vprintf_like_t for esp_log_set_vprintf */
uint32_t port_console_dropped_lines(void);

void     port_writer(void *ud, const char *chan, size_t chan_len, const char *msg, size_t msg_len);
void     port_diag(struct UVM *vm, void *ud, int level, const char *msg, size_t len);

/* The event queue: the one way the camera task and the button interrupt
 * reach the VM.  Only the task that calls port_queue_drain injects, so
 * the runtime's single-producer ring sees one producer. */
typedef struct {
    urbi_event_id_t id;
    uint64_t        payload;    /* 0 = no payload; else the first 8 bytes the script receives as an Integer */
    uint8_t         has_payload;
} port_event_rec;
int      port_queue_init(size_t depth);                        /* 0 ok, -1 no memory */
int      port_queue_post(const port_event_rec *rec);           /* from a task; 0 ok, -1 full (counted, never blocks) */
int      port_queue_post_from_isr(const port_event_rec *rec);  /* from an interrupt; same contract */
/* Injects every queued record and returns the count.  The record's payload
 * buffer is always passed; with has_payload clear it goes with n = 0, so
 * the runtime copies nothing. */
size_t   port_queue_drain(struct UVM *vm);
int      port_queue_wait(uint32_t timeout_ms);                 /* blocks until a record is posted or the timeout; 1 posted, 0 timeout */
uint32_t port_queue_dropped(void);
uint32_t port_queue_inject_failures(void);                     /* failed urbi_inject_event calls during drain */

/* The USB Serial/JTAG byte stream as a UTransport.  The driver's write is
 * all-or-nothing against its TX ring, so the transport's write sends at
 * most tx_buf bytes per call and the REPL service re-offers the rest.  A
 * console envelope can only land between two such chunks, so a tx_buf at
 * least as large as the session's output_buf_cap keeps a REPL frame from
 * being split around a console line.  The raw write (the console sink)
 * refuses anything longer than tx_buf, so console lines longer than the
 * TX buffer are always dropped. */
int      port_usb_serial_jtag_init(size_t rx_buf, size_t tx_buf, uint32_t write_deadline_ms);  /* 0 ok */
void     port_usb_serial_jtag_vtable(UTransport *out);
int      port_usb_serial_jtag_raw_write(const void *buf, size_t n);  /* the console sink: n or 0 */

#ifdef __cplusplus
}
#endif
#endif
