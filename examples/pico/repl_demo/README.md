# urbi-embedded v0.9.4 — Raspberry Pi Pico interactive REPL demo

A complete bring-up of urbi-embedded on the Raspberry Pi Pico (RP2040 /
Cortex-M0+).  Connects an interactive urbiscript REPL over **USB CDC**
or **UART0** to a stock Pico with no extra wiring beyond the BOOTSEL
button (already on the board) and the on-board user LED on GPIO 25.

## What you get on the board

A 264 KB SRAM Cortex-M0+ running:

- the full urbi-embedded interpreter and stdlib,
- an NDJSON REPL service on USB CDC (`/dev/ttyACM0`) and UART0
  (`GP0` = TX, `GP1` = RX, 115200 8N1),
- four BSP fixtures exposed to script:
  - **LED** — `led_on()`, `led_off()`, `led_toggle()`, `led_pwm(duty)`
  - **Temperature** — `temp_celsius()` returns the on-die ADC4 reading
  - **Button** — `button_pressed()` queries BOOTSEL; `pressed` event
    fires on each rising edge (debounced @ 10 Hz)
  - **Tick** — `tick` event fires every 100 ms (TIMER_IRQ_0)
- the default-installed `repl_demo.u` workload (LED-on-button +
  temperature-threshold watchers).

> The v0.7.1 embedding API only exposes **flat** top-level realm-globals
> via `urbi_register`.  Sub-object syntax like `Lobby.led.on()` is a
> v1.x property-installer item.  All BSP verbs are flat names.

## Build

### Prerequisites

1. **Pico SDK** somewhere on disk; point `PICO_SDK_PATH` at it (env
   var or `-DPICO_SDK_PATH=...` on the cmake invocation).  The default
   in `CMakeLists.txt` looks four levels up from the example —
   `../../../../tools/pico-sdk`, i.e. alongside the `urbi-embedded/`
   tree in the urbi workspace.
2. **arm-none-eabi-gcc** on `$PATH`.
3. **cmake** ≥ 3.13 and a recent **make** (`gmake` on \*BSD).

### Cross-build `liburbi.a`

From the repo root:

```sh
make cross-pico URBI_ENABLE_REPL=1
```

This produces `build/arm-cortex-m0plus/liburbi.a` (the IMPORTED target
the example's `CMakeLists.txt` references) and also builds
`tools/urbi-compile-stdlib-pico` as a side-effect (the bake tool that
turns `repl_demo.u` into a C header at example-configure time).

> **Why `URBI_ENABLE_REPL=1`?**  Without it, the compiler frontend
> (`src/lex`, `src/parse`, `src/emit`) and the REPL infrastructure
> aren't linked into `liburbi.a`, and the example would link-error on
> `urbi_repl_serve_init` / `urbi_repl_eval`.

### Build the example

```sh
cd examples/pico/repl_demo
mkdir build && cd build
cmake ..
make
```

Outputs:

- `repl_demo.elf` — symbol-bearing ELF (gdb / openocd).
- `repl_demo.uf2` — drag-drop-flashable image for BOOTSEL mode.
- `repl_demo.hex` / `.bin` / `.dis` — convenience artifacts.

## Flash

1. Hold **BOOTSEL** on the Pico while plugging USB in (or pressing the
   RESET line if you have a debug probe).
2. The Pico enumerates as a USB mass-storage volume (`RPI-RP2`).
3. Drag `repl_demo.uf2` onto it.
4. The Pico reboots, USB re-enumerates as a CDC ACM device, and the
   demo is live.

## Connect

### Option A: USB CDC (`/dev/ttyACM0`)

```sh
picocom -b 115200 --omap crlf --imap lfcrlf /dev/ttyACM0
# or:
screen /dev/ttyACM0 115200
# or, with NDJSON awareness, the urbi-send / urbi-recv host tools shipped
# built from the urbi-embedded tools/ directory.
```

### Option B: UART0 via USB-serial adapter

Wire a 3.3V USB-serial adapter:

| Pico pin | Adapter |
|---------:|:--------|
| GP0      | RX      |
| GP1      | TX      |
| GND      | GND     |

```sh
picocom -b 115200 --omap crlf --imap lfcrlf /dev/ttyUSB0
```

Either path lands on the same NDJSON REPL — the urbi-embedded REPL
service treats each connected transport as an independent session.

## Sample session

Lines starting with `>` are typed by you; the response immediately
follows.  All REPL lines are NDJSON-framed on the wire; the surface
shown here is the human-friendly form a frontend would render.

```
urbi v0.9.4-pico booting
urbi-embedded v0.9.4 on Raspberry Pi Pico
verbs: led_on/off/toggle, led_pwm(d), temp_celsius(), button_pressed()
events: tick / pressed
idiom: heartbeat: { every (P) X }; heartbeat.stop()  (Tag.new deferred v1.x)
type 'greet.stop()' / 'temp_watch_hot.stop()' to disable defaults

> led_on()
nil
> temp_celsius()
22.4
> button_pressed()
false
> heartbeat: { every (500ms) led_toggle() }
nil
> // ... LED blinks at 2 Hz, observed visually ...
> heartbeat.stop()
nil
> at (temp_celsius() > 30.0) echo("warm!")
nil
> // ... touching the chip with a finger warms it past 30 within seconds ...
warm!
> greet.stop()        // disable the default LED-on-button watcher
nil
```

## Footprint expectations

| component                  | Flash | SRAM |
|----------------------------|------:|-----:|
| pico-sdk + TinyUSB         | ~64 K | ~12 K |
| urbi-embedded core         | ~80 K | ~20 K |
| Baked `repl_demo.u`        |  ~1 K |     — |
| BSP + main                 |  ~6 K |  ~1 K |
| Runtime heap (newlib pool) |     — | ~200 K available |

Numbers are approximate from a `-Os` build with the v0.9.4 cross-pico
defaults.  Exact figures depend on which urbi features the workload
exercises (every-loop tags allocate; watchers use a 16-slot pool).

## Known issues / v1.x deferrals

- **Sub-object slot installers** (`Lobby.led.on()` style).  The v0.7.1
  embedding API exposes only flat top-level globals; binding a host-fn
  as `Lobby.led.on()` requires a property-installer API that's slated
  for v1.x.  The demo uses flat names instead (`led_on()` etc.).
- **`Tag.new()`** for script-side dynamic tag creation.  v0.9.4 supports
  label-prefix tag binding (`mytag: { ... }`) and runtime cancel via
  `.stop()`, but the `Tag.new()` constructor isn't wired yet — tag
  variables are introduced solely via the label-prefix surface.
- **Bare-prefix tag labels** require brace-block bodies — `mytag:
  whenever (E) ...` parses as a no-op label followed by a top-level
  watcher (the watcher is created, but the tag never binds to it).
  Always use `mytag: { whenever (E) ... }`.  v1.x lexer-level dangling-
  statement lookahead is design-risks-backlog.
- **Closure-body capture in `every (P) body`** has a v0.9.1 closure-
  bare-name resolution gap (`Lobby.echo` inside an `every` body can't
  resolve unqualified `__builtin_lobby_send`; explicit `Lobby.` prefix
  workaround).  Surfaces with REPL-installed `every` loops that try to
  print via `echo` — affected idioms aren't load-bearing in the demo
  defaults.
- **Two-transport simultaneity untested.**  Each transport is wired
  independently and the dispatcher supports multiple sessions per
  v0.9.1, but the demo has only been driven through one transport at
  a time during bring-up.  Mixing USB CDC and UART concurrently may
  surface latent ordering bugs.

## File layout

```
examples/pico/repl_demo/
├── CMakeLists.txt            # build script (pico-sdk + urbi)
├── pico_sdk_import.cmake     # vendored from pico-sdk/external/
├── repl_demo.u               # default workload (baked into header)
├── README.md                 # this file
└── main/
    ├── main.c                # entry point + main loop
    └── bsp/
        ├── bsp_register.{c,h}    # wires all four fixtures
        ├── bsp_led.{c,h}         # GPIO 25 + PWM slice
        ├── bsp_temp.{c,h}        # on-die ADC4
        ├── bsp_button.{c,h}      # BOOTSEL polling + debounce
        └── bsp_tick.{c,h}        # TIMER_IRQ_0 @ 100 ms
```
