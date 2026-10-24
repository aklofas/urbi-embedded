# ESP32-S3-EYE eye demo

## What this is

This example tracks a coloured object with the ESP32-S3-EYE's OV2640 camera and draws a crosshair on it on the board's 240x240 LCD, with the tracking logic in urbiscript.
The camera task finds the blob in C and posts it to a queue; the task that owns the VM drains the queue into events, and the workload moves the crosshair, counts the blobs, and cycles the target colour on each BOOT press.
The workload is [`main/eye_demo.u`](main/eye_demo.u); it is compiled on the host at build time, baked into the firmware as bytecode, and loaded into the main realm at boot.

A REPL session runs on the board's USB Serial/JTAG port beside the workload.
Once `app_main` starts, every line on that port is one JSON object: the session's replies, and the board's own console text as `{"kind":"output","channel":"console","msg":"..."}` envelopes.

The board exposes five verbs on `Lobby`:

- `draw_crosshair(x, y)` moves the crosshair; both coordinates are clamped to 0..239.
- `set_target_chroma(dominant, dominance, min_bright)` sets the colour the camera matches: the dominant channel (0 red, 1 green, 2 blue) must beat the other two by `dominance` and reach `min_bright`, on a 5-bit scale.
- `log(msg)` prints one console line.
- `heap_live()` returns the bytes the VM holds from its arena.
- `events_dropped()` returns the camera and button records lost to a full queue or a refused injection.

It also exposes three events on `Object`:

- `blob_seen` carries one Integer: x in bits 0-15, y in bits 16-31, and the blob's pixel count from bit 32.
- `button_pressed` fires on every falling edge of the BOOT button, bounces included.
- `stats_tick` is registered for a session to use; the board does not emit it.

The workload's watchers and its stats line run under the tag `boot`, on `Object`, so a session can stop them and take the board over.

## Build

Prerequisites:

- ESP-IDF v6.0.1 and its tools. The repository Makefile expects them at `../tools/esp-idf` and `../tools/esp-idf-tools` beside the repository; set `IDF_PATH` and `IDF_TOOLS_PATH` to use another install.
- A host C compiler, for the host `urbi` that compiles the workload.

From the repository root:

```sh
make esp32s3-eye-demo
```

That one command does, in order:

1. Builds the host `urbi`.
2. Builds `liburbi.a` for the ESP32-S3 with the IDF's own `xtensa-esp32s3-elf-gcc`, so the archive and the firmware come from one toolchain.
3. Runs `idf.py build` on this example, which compiles `main/eye_demo.u` into `eye_demo_baked.h`, builds the `esp32-idf` component and links the archive.
4. Prints the firmware's `size` line.

The firmware is `build/eye_demo.elf` under this directory, with the flashable images beside it.

The camera can be turned off for an S3 board without the sensor: set **Eye Demo Configuration → Enable the OV2640 camera** off with `idf.py -C examples/esp32/eye_demo menuconfig`, or set `CONFIG_EYE_DEMO_ENABLE_CAMERA=n`.
The display, the button, the workload and the session still run; no `blob_seen` arrives.

## Flash

With the board on its USB port:

```sh
make esp32s3-flash PORT=/dev/ttyACM0
```

or, inside the IDF environment:

```sh
idf.py -C examples/esp32/eye_demo -p /dev/ttyACM0 flash
```

## Connect

```sh
picocom -b 115200 /dev/ttyACM0
```

Do not use `idf.py monitor`: it expects plain text and would show the raw JSON.
Leave picocom with C-a C-x.

Type one JSON request per line:

```json
{"id":1,"op":"eval","code":"1+1"}
```

Things to try:

- `echo("hi")`, `1+1` and `Tracker.count` answer at once.
- `boot.stop()` stops the workload's watchers: the crosshair freezes and the stats line stops.
- `at (blob_seen?(var p)) echo(p.and(65535))` from the session prints each blob's x and resumes tracking in the session's hands; `draw_crosshair(p.and(65535), p.shr(16).and(65535))` in the body moves the crosshair again.

## Console lines

The shim prints a banner with the runtime version, the arena line, one line per boot stage (`[1] urbi_open... ok` through `[5] peripherals... ok`), then the heap lines:

```text
arena <n> B, free internal heap <n> B
boot heap: alloc live <n> B, gc live <n> B, heap top <n> B, budget <n> B, cycles <n>
ready: alloc live <n> B, gc live <n> B, heap top <n> B, budget <n> B, cycles <n>
free internal heap <n> B
target: red
stats: count <n>, target <name>, live <n> B, dropped <n>
```

- `arena` is the runtime's static arena; `free internal heap` is what the IDF heap has left in internal SRAM at that moment, the headroom beside the arena. It prints again after `ready:`, once the peripherals hold their memory.
- `alloc live` is the bytes the VM has requested from its allocator and not yet freed, read right after a full collection.
- `gc live` is the collector's own view of what it holds live.
- `heap top` is the arena's high-water mark. It includes the allocator's block headers, so it reads higher than `alloc live`.
- `budget` is the heap figure the collector paces against: three quarters of the arena, 98,304 B by default.
- `cycles` is the number of collections so far.

`boot heap:` prints once the VM is open with its standard library, and `ready:` once the verbs, the events, the workload, the session and the peripherals are up.
`target:` prints when the workload loads and on each press that changes the target.
`stats:` prints every two seconds from the workload: the blobs seen, the current target, the VM's live bytes (not collected first, so it moves with the collector), and the records dropped.
Runtime diagnostics print as `[E] msg`, `[W] msg`, `[I] msg` or `[D] msg`, and ESP-IDF log lines arrive framed the same way.
An ESP-IDF log line over 255 bytes is truncated and sent; a console line whose escaped form outgrows the 512 B frame buffer is dropped whole and counted.

## Definition of done for this port

A pasted picocom log plus the owner's word on the LCD:

1. `boot heap: alloc live <n> B` under 49,152 B.
2. The LCD shows the camera feed with a crosshair following a coloured object; the stats line's count rises.
3. The BOOT button cycles the target colour; the stats line names the new target.
4. From picocom: `echo("hi")`, `1+1`, `Tracker.count` answer; `boot.stop()` freezes the crosshair; `at (blob_seen?(var p)) draw_crosshair(p.and(65535), p.shr(16).and(65535))` from the session resumes it.
5. Five minutes of tracking: the stats line's live bytes flat within 1 KB of the first; no `OutOfMemoryError`, no reset, no task-watchdog warning. (A deliberately runaway eval such as `loop {}` from the session is not part of this item: it trips the watchdog by design, see the known limits.)

`blob_seen` carries one packed Integer, so item 4's session watcher binds one variable and unpacks it.

## Memory layout

- The VM's heap is a 128 KB arena in `.bss` of internal SRAM (`CONFIG_URBI_ARENA_BYTES`, 131,072 B by default), served by the component's bump-and-freelist allocator.
- The arena and the display's 115,200 B framebuffer share the 341,760 B internal DRAM segment, so a larger arena does not link without moving something out.
- The collector's budget is three quarters of the arena, 98,304 B; the rest covers the allocator's block headers and the memory the collector does not track.
- The camera's two V4L2 frame buffers are in the 8 MB octal PSRAM, where esp_video places them.
- The display's 240x240 RGB565 framebuffer is a static in internal SRAM, so the SPI DMA reads it directly.
- The VM task has a 32 KB stack from the IDF heap; the camera and display tasks have 4 KB static stacks.
- The USB Serial/JTAG driver holds a 1 KB receive ring and an 8 KB transmit ring, the transmit ring as large as the session's output buffer.

## Measured

| What | Figure |
| --- | --- |
| `liburbi.a` for the ESP32-S3, hosted, text / data / bss | 166,340 / 8 / 4 B |
| `liburbi.a` for the ESP32-S3, bytecode-only, text / data / bss | 95,412 / 8 / 4 B |
| Firmware `eye_demo.elf`, text / data / bss | 360,169 / 135,046 / 1,101,871 B |
| Firmware `eye_demo.bin` | 495,328 B |
| Undefined symbols in the ELF | 3, all IDF linker hooks (none from the runtime, the port or the demo) |
| Boot heap (qemu), alloc live / gc live / heap top | 48,748 / 26,992 / 71,768 B of a 131,072 B arena (budget 98,304 B) |
| After one session request (qemu smoke, not this workload), alloc live / gc live / heap top | 50,249 / 27,752 / 75,120 B |
| Task stack high water (qemu smoke, not this workload) | 24,440 B used of 65,536 B; `URBI_TASK_STACK` set to 32 KB |
| Boot heap and live bytes over five minutes (board) | from the board log |

The qemu smoke loads a one-line workload, so it gives no figure for this demo's `ready:` line, which also counts the verbs, the events, `eye_demo.u` and the session.
Record the `ready:` line in the board log; there is no cap on it at this tag.

## Known limits

- The bootloader's output, the ESP-IDF startup log before `app_main` installs the console hook, and a panic's backtrace are not framed: they reach the port as plain text before or instead of the JSON lines.
- A continuously busy VM yields one tick every ~100 ms so the idle task and the watchdog stay fed. A session eval is the exception: it runs to completion inside one service step, so an eval longer than 5 s, or any eval while some strand never stops being runnable, trips the task watchdog, whose warning reaches the port as plain text.
- With no host reading the USB port, each console line waits up to the 100 ms deadline (in practice up to about twice that) and a pending reply stalls the VM loop by that much per iteration.
- The port is always connected: closing picocom does not end the session, and a new picocom continues the same one.
- The OV2640's auto exposure and white balance follow the room light; a target that tracks well in daylight may need its thresholds in `eye_demo.u` retuned under warm indoor light.
- The display is 240x240, so `draw_crosshair` clamps both coordinates to 0..239; a session that passes a point off the panel sees the crosshair on the edge.

## File layout

```text
examples/esp32/eye_demo/
├── CMakeLists.txt              # the IDF project
├── partitions.csv              # 8 MB flash, one factory app
├── sdkconfig.defaults          # target, PSRAM, console, arena size, camera
├── README.md                   # this file
└── main/
    ├── CMakeLists.txt          # the main component and the bake step
    ├── Kconfig.projbuild       # the camera switch and the arena size
    ├── idf_component.yml       # esp_video and esp_cam_sensor, pinned
    ├── eye_demo_main.c         # the VM task, the verbs, the heap lines, the loop
    ├── eye_demo.u              # the workload
    ├── eye_camera.c/.h         # esp_video capture, the blob detector, the queue posts
    ├── eye_display.c/.h        # the ST7789 panel and the crosshair
    ├── eye_button.c/.h         # the BOOT button interrupt
    ├── crosshair.h             # the crosshair overlay, host-testable
    ├── detect_blob.h           # the blob result type
    └── detect_blob_chroma.h    # the chrominance detector, host-testable

components/esp32-idf/           # the port: arena, clock, console framing, queue, USB Serial/JTAG transport
```
