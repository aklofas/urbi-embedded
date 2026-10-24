# esp32-idf

ESP-IDF port glue for urbi on the ESP32-S3, used by the ESP32-S3-EYE demo
under `examples/esp32/eye_demo/`.

## What this is

Two things live here: the port sources under `src/port/` (allocator,
clock, console framing, writer and diag hooks, event queue, USB
Serial/JTAG transport), and the component wrapper that hands an ESP-IDF
project the runtime archive the repository Makefile builds.

It is not a standalone `idf.py` component.  The runtime is compiled by
the repository's own build with the `xtensa-esp32s3` preset, from the
same source lists and flags as every other target, and the stdlib is
baked by the host `urbi` binary.  Neither step is something an IDF
component can do on its own, so the component links a prebuilt archive
instead of compiling the runtime sources itself.

## Build

From the repository root:

```sh
make esp32s3-eye-demo
```

The demo project finds this component through `EXTRA_COMPONENT_DIRS`
pointing at `components/`.  Two CMake cache entries tell the component
where the repository build put its outputs:

| Cache entry       | What it names                               |
|-------------------|---------------------------------------------|
| `LIBURBI_ARCHIVE` | the `liburbi.a` built for `xtensa-esp32s3`  |
| `URBI_HOST_BIN`   | the host `urbi` binary that bakes bytecode  |

## Public API

`src/port/port_esp_idf.h` declares everything, in five groups:

- **Arena.** `port_alloc` is the allocator hook, over a static arena of
  `URBI_ARENA_BYTES` in internal SRAM (`CONFIG_URBI_ARENA_BYTES` from the
  project's Kconfig).  The `port_alloc_*` accessors report live bytes,
  the high-water mark and the counters.
- **Clock.** `port_time_us` is the clock hook, backed by
  `esp_timer_get_time`.
- **Console.** Every line printed by the writer (`port_writer`), the diag
  hook (`port_diag`) and the ESP_LOG hook (`port_console_vprintf`) leaves
  as one NDJSON output envelope on the `console` channel.  A line the
  sink cannot take whole is dropped whole and counted.
- **Queue.** `port_queue_post` and `port_queue_post_from_isr` hand events
  to the VM without blocking; a full queue drops and counts.
  `port_queue_drain` injects every queued record into the runtime.
- **Transport.** `port_usb_serial_jtag_vtable` fills a `UTransport` for
  the REPL service; `port_usb_serial_jtag_raw_write` is the console sink.

## Threading

One task owns the VM: it steps it, drains the queue and is the only
caller of `urbi_inject_event`.  Every other task and every interrupt
reaches the VM by posting to the queue.

The console has two paths.  The staging calls (`port_console_begin`,
`port_console_part`, `port_console_end`, and `port_console_write` and
`port_console_puts`, which compose them) share one static buffer and are
for the VM task only.  `port_console_vprintf` formats and frames on its
own stack and hands the envelope straight to the sink, so it is safe
from any task; that is why it can be installed with
`esp_log_set_vprintf`.  The sink is all-or-nothing, so envelopes from
different tasks reach the wire whole.

The USB Serial/JTAG TX buffer should be at least as large as the REPL
session's output buffer, so a REPL frame is never split around a console
line.  Console lines longer than the TX buffer are always dropped.
