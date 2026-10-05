# Raspberry Pi Pico eval-service demo

## What this is

This example runs the urbiscript eval service on a stock Raspberry Pi Pico (RP2040, Cortex-M0+), with one session on USB CDC and one on UART0.
The board exposes six verbs (`led_on()`, `led_off()`, `led_toggle()`, `led_pwm(duty)`, `temp_celsius()`, `button_pressed()`), two events (`pressed` on each BOOTSEL press and `tick` every 100 ms), and the `boot` tag that covers everything the boot workload installs.
The boot workload is [`repl_demo.u`](repl_demo.u); it is compiled on the host at build time, baked into the firmware as bytecode, and loaded into the main realm before the service starts.

## Build

Prerequisites:

- xpack `arm-none-eabi-gcc` 14.2.1 on `PATH`.
- CMake 3.13 or later, and `make`.
- pico-sdk 2.2.0, either at `PICO_SDK_PATH` or at `../tools/pico-sdk` beside the repository.
- Network access for the first configure: pico-sdk fetches and builds `picotool` once.

From the repository root:

```sh
make pico-repl-demo
```

That one command builds the host `urbi` (which compiles `repl_demo.u` to bytecode), the hosted Cortex-M0+ `liburbi.a` (newlib present, so floats print), and then configures and builds this example with CMake.
It ends by printing the firmware's `size` line.

Outputs, under `examples/pico/repl_demo/`:

- `build/repl_demo.elf`, the ELF with symbols, for a debug probe.
- `build/repl_demo.uf2`, the image to copy onto the board.

## Flash

1. Hold **BOOTSEL** while plugging the Pico into USB.
2. The board mounts as a mass-storage volume named `RPI-RP2`.
3. Copy `build/repl_demo.uf2` onto it.
4. The board reboots into the demo and enumerates as a USB CDC device, usually `/dev/ttyACM0`.

## Connect

```sh
picocom -b 115200 --omap crlf --imap lfcrlf /dev/ttyACM0
```

`--omap crlf` turns Enter into the newline the service frames requests on.
`--imap lfcrlf` renders the board's bare newlines as line breaks.

After USB enumerates, the LED stays on for three seconds before the banner prints.
Start picocom inside that window to see the whole boot.
Leave picocom with C-a C-x.

UART0 is a second, independent session, registered once the boot workload has loaded and kept for the life of the firmware.
Wire a 3.3 V USB-serial adapter to GP0 (Pico TX), GP1 (Pico RX) and GND, at 115200 8N1.

## The wire

Each request is one JSON object on one line, with an `id`, an `op` of `eval`, and the urbiscript text in `code`.
An eval answers with zero or more `output` lines, then one `result` line carrying the value as a JSON string (or an `error` line with a `code` and `message`), then a `done` line.
Output a watcher or timer prints after its eval's `done` arrives without an `id`.

Copy-paste lines:

```json
{"id":1,"op":"eval","code":"echo(\"hi\")"}
{"id":2,"op":"eval","code":"1+1"}
{"id":3,"op":"eval","code":"temp_celsius()"}
{"id":4,"op":"eval","code":"boot.stop()"}
{"id":5,"op":"eval","code":"at (pressed?) led_toggle()"}
{"id":6,"op":"eval","code":"var t = Tag.new(\"t\") | t: every (1s) echo(temp_celsius())"}
{"id":7,"op":"eval","code":"t.stop()"}
```

## Console lines

The shim prints its own lines on both channels, in a different form on each.

- **UART0** is the human and debug channel. Console lines go out as plain text and interleave with the UART session's JSON.
- **USB CDC** carries plain text only before a session opens on it, which is the boot banner. Once a session is open, every line on USB CDC is NDJSON: each console line arrives as an output envelope on the `console` channel, such as `{"kind":"output","channel":"console","msg":"idle: alloc live ...\r\n"}`.
- A console line is never written into the middle of a session's half-sent JSON line on USB CDC. When the session has output part-way out, the USB copy of that console line is skipped; UART0 still gets it.

The heap lines read:

```text
boot heap: alloc live <n> B, gc live <n> B, heap break <n> B, budget <n> B, cycles <n>
ready: alloc live <n> B, gc live <n> B, heap break <n> B, budget <n> B, cycles <n>
session open: alloc live <n> B, gc live <n> B, heap break <n> B, budget <n> B, cycles <n>
session closed: alloc live <n> B, gc live <n> B, heap break <n> B, budget <n> B, cycles <n>
idle: alloc live <n> B, gc live <n> B, heap break <n> B, budget <n> B, cycles <n>
```

Each line is read right after a full collection.

- `alloc live` is the bytes the VM has requested from its allocator and not yet freed. This is the figure the 48 KB boot target caps.
- `gc live` is the collector's own view of what it holds live, usually the smaller figure.
- `heap break` is how far newlib's heap has grown, its high-water mark. It includes newlib's chunk headers and the allocator's size headers, so it reads higher than `alloc live`.
- `budget` is the heap figure the collector paces against.
- `cycles` is the number of completed collections.

`boot heap:` prints once the VM is open with its standard library, and `ready:` once the workload, the service and the tick are up.
`session open:` and `session closed:` print when a host opens and drops the USB port.
`idle:` prints every 30 s.

## Definition of done for this port

Run these in picocom on `/dev/ttyACM0`, in order.

1. **Boot heap.** The `boot heap:` line shows `alloc live` under 49,152 B.
2. **Three evals.** Type each line and look for its answer:
   - `{"id":1,"op":"eval","code":"echo(\"hi\")"}` gives an `output` line whose `msg` ends in `hi\n`, then `"value":"nil"` and `done`.
   - `{"id":2,"op":"eval","code":"1+1"}` gives `"value":"2"`.
   - `{"id":3,"op":"eval","code":"temp_celsius()"}` gives a float, such as `"value":"27.4"`.
3. **Watcher from the session.** Send `{"id":4,"op":"eval","code":"boot.stop()"}` and press BOOTSEL: the LED does not toggle.
   Then send `{"id":5,"op":"eval","code":"at (pressed?) led_toggle()"}` and press BOOTSEL: the LED toggles on each press.
4. **Periodic print.** Send `{"id":6,"op":"eval","code":"var t = Tag.new(\"t\") | t: every (1s) echo(temp_celsius())"}`: an `output` line with a temperature arrives once a second.
   Send `{"id":7,"op":"eval","code":"t.stop()"}`: the lines stop.
5. **Session reopen.** Note the `alloc live` figure on the `session open:` line, leave picocom with C-a C-x, and start it again.
   The new `session open:` line's `alloc live` is within 1,024 B of the first.
6. **Idle.** Leave the board alone for a minute: two consecutive `idle:` lines, 30 s apart, show the same `alloc live`.

## Memory layout

[`memmap_repl_demo.ld`](memmap_repl_demo.ld) puts a 32 KB stack at the top of SRAM, from `__StackLimit` (0x20038000) to `__StackTop` (0x20040000).
newlib's heap runs from `end`, just past the statics, up to `__StackLimit`, and the VM allocates from it through `realloc`.
Each block the VM allocates carries a 16-byte header holding its requested size, which is how `alloc live` is counted; on this core that is a 4-byte size padded to the 8-byte alignment of a `double`.

The collector's budget is that heap less a 16 KB session reserve.
The reserve covers what the eval service and newlib allocate outside the VM's allocator: each session's 4 KB output staging, its input line, and the service's own structures.

Every session compiles under a budget of 24 parser levels, 2,000 AST nodes and 4 KB of source.
One parser level costs about 1 KB of stack on this core, so 24 levels leave 8 KB of the 32 KB stack for everything else.
An AST node is 56 bytes, so 2,000 nodes is a 112 KB transient at worst, which the 4 KB source cap makes unreachable in practice.

## Measured

| What | Figure |
| --- | --- |
| Session boot, host 64-bit build | 71,273 B |
| After the first session close, host 64-bit build | 71,420 B |
| After the second session close, host 64-bit build | 71,420 B |
| Hosted Cortex-M0+ `liburbi.a`, text+data+bss | 146,760 B |
| Firmware `repl_demo.elf`, text / data / bss | 280,136 / 0 / 4,428 B |
| Firmware `repl_demo.uf2` | 552,448 B |
| Boot heap (board) | from the board log |
| Session open (board) | from the board log |
| Idle growth (board) | from the board log |

The firmware's initialized data is counted under text by `arm-none-eabi-size`, because the SDK's `.data` section carries code flags.

## Known limits

- The service refuses a request line only past its 1 MiB framing cap, which is not configurable, so a single line that long exhausts the heap first; connect only over trusted serial links.
- TinyUSB exposes one CDC interface, so there is one USB session at a time.
- UART input is noticed as it arrives: the receive interrupt moves it into a 256-byte ring and wakes the main loop. Bytes that arrive with the ring full are dropped.
- Floats are `double`, computed through libgcc's soft-float routines on this core.

## File layout

```text
examples/pico/repl_demo/
├── CMakeLists.txt            # the build: pico-sdk, the bake step, the link
├── bake.cmake                # compiles repl_demo.u to repl_demo_baked.h
├── memmap_repl_demo.ld       # linker script: 32 KB stack at the top of SRAM
├── pico_sdk_import.cmake     # copied from pico-sdk/external/
├── repl_demo.u               # the boot workload
├── README.md                 # this file
└── main/
    ├── main.c                # boot, console, main loop
    ├── tusb_config.h         # TinyUSB device configuration
    ├── usb_descriptors.c     # one CDC interface
    ├── transport_usb_cdc.{c,h}   # the USB CDC session transport
    ├── transport_uart.{c,h}      # the UART0 session transport and its receive ring
    └── bsp/
        ├── bsp_register.{c,h}    # installs the verbs and events
        ├── bsp_led.{c,h}         # GPIO 25 and its PWM slice
        ├── bsp_temp.{c,h}        # on-die temperature sensor (ADC4)
        ├── bsp_button.{c,h}      # BOOTSEL polling and debounce
        └── bsp_tick.{c,h}        # the 100 ms timer tick
```
