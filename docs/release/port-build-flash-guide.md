# Clone → Build → Demo (v1.0)

This is the third-party "does it work from a fresh clone" path for each shipped
port. The goal is **≤ 5 minutes from clone to a running demo** on each
architecture. Both the host-side build of all firmware and hardware flashing
are manual and documented per-architecture below.

**The Raspberry Pi Pico builds from this tree with `make
pico-repl-demo`; ESP32-S3 and STM32F429I-DISC1 are parked until their
own tags.** The core re-foundation replaced the runtime the latter two
were brought up against, and neither's component manifest /
`examples/` workload has been rebuilt on it. Section 1 (the Linux
host) builds and runs today; section 2 (the Pico) builds and flashes
from this tree; the steps in sections 3-4 are kept as a record of what
worked at each port's own ship tag, not as commands that build against
the tree as it stands. The generic Cortex-M0+/M4F/M7/RISC-V archives
build and are footprint-measured on every push through the cross
presets in [the build system doc](../internals/build-system.md); the
Pico example wires in its own hosted shape of the Cortex-M0+ archive,
and nothing wires one into the other two examples yet.

```sh
git clone <repo-url> urbi-embedded
cd urbi-embedded
```

Work through the per-architecture steps below on a pristine tree (no stale
`build/` artifacts from a prior clone).

## Prerequisites (per architecture)

| Target | Toolchain | SDK / HAL | Flash tool |
|--------|-----------|-----------|------------|
| Linux host REPL | any C99 `cc` (gcc/clang) | — | — |
| Raspberry Pi Pico (RP2040) | `arm-none-eabi-gcc` (xpack 14.2.1) + CMake 3.13+ | pico-sdk 2.2.0 (`PICO_SDK_PATH`, default `../tools/pico-sdk`) | drag-drop `.uf2` (BOOTSEL) |
| ESP32-S3 | ESP-IDF v6.0.1 (`IDF_PATH`, `. $IDF_PATH/export.sh`) | bundled in IDF | `idf.py flash` |
| STM32F429I-DISC1 | `arm-none-eabi-gcc` (xpack 14.2.1) | STM32CubeF4 v1.28.2 headers (vendored) | `st-flash` (stlink) |

## 1. Linux host REPL (30-second quickstart)

```sh
make                       # build liburbi.a + the urbi binary
echo "1 + 2" | ./build/host/urbi -i      # -> [..........] 3
./build/host/urbi -i                      # interactive REPL
```

## 2. Raspberry Pi Pico — `examples/pico/repl_demo`

```sh
make pico-repl-demo            # host urbi + hosted Cortex-M0+ archive + CMake build
# -> examples/pico/repl_demo/build/repl_demo.uf2
```

Flash: hold **BOOTSEL**, plug USB, drag `repl_demo.uf2` onto the `RPI-RP2`
volume. Connect: `picocom -b 115200 --omap crlf --imap lfcrlf /dev/ttyACM0`.
Each request is one JSON object per line; `{"id":1,"op":"eval","code":"1+1"}`
answers with `"value":"2"`. Full build, flash, wire and known-limits detail
is in [the example's own README](../../examples/pico/repl_demo/README.md).

## 3. ESP32-S3 — `examples/esp32/eye_demo` (parked until its own tag)

```sh
. $IDF_PATH/export.sh
cd examples/esp32/eye_demo && idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The ~330-line eye demo runs a blob-detection loop; the BOOT button cycles the
RGB LED (RED→GREEN→BLUE) tracking targets and the LCD shows a crosshair overlay.

## 4. STM32F429I-DISC1 — `examples/stm32f4/mandelbrot` (parked until its own tag)

```sh
cd examples/stm32f4/mandelbrot && make
# -> build/mandelbrot.bin
st-flash write build/mandelbrot.bin 0x8000000
```

Reset; connect the ST-Link VCP (115200 8N1). The ILI9341 LCD renders the
Mandelbrot set with progressive 32→1 tile refinement; tilting the board pans the
view (gyro) and the USER button zooms 2×. Float values are `double` on every
target, so there is no float-width build flag to keep consistent between the
application and the library.

## Notes

- Skip the ESP32-S3 step cleanly when `IDF_PATH` is unset (its build is
  environment-heavy); work through the other three regardless.
- A port that fails to build from a fresh tree is a release blocker (it would
  also block the Track A hardware regression).
