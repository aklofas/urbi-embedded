# Clone → Build → Demo (v1.0)

This is the third-party "does it work from a fresh clone" path for each shipped
port. The goal is **≤ 5 minutes from clone to a running demo** on each
architecture. Both the host-side build of all firmware and hardware flashing
are manual and documented per-architecture below.

**The Raspberry Pi Pico builds from this tree with `make
pico-repl-demo`; the STM32F429I-DISC1 builds from this tree with `make
stm32f4-mandelbrot`; ESP32-S3 is parked until its own tag.** The core
re-foundation replaced the runtime ESP32-S3 was brought up against,
and its component manifest / `examples/` workload has not been
rebuilt on it. Section 1 (the Linux host) builds and runs today;
sections 2 and 4 (the Pico and the STM32F4) build and flash from this
tree; the steps in section 3 are kept as a record of what worked at
ESP32-S3's own ship tag, not as commands that build against the tree
as it stands. The generic Cortex-M0+/M4F/M7/RISC-V archives build and
are footprint-measured on every push through the cross presets in
[the build system doc](../internals/build-system.md); the Pico example
wires in its own hosted shape of the Cortex-M0+ archive and the STM32F4
example wires in the `arm-cortex-m4f` preset's bytecode-only shape,
and nothing wires one into the ESP32-S3 example yet.

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
| STM32F429I-DISC1 | `arm-none-eabi-gcc` (xpack 14.2.1) | STM32CubeF4 v1.28.2 (`HAL_ROOT`, default `../tools/stm32cube-f4`) | STM32CubeCLT's `STM32_Programmer_CLI`, or `st-flash` (stlink) |

## 1. Linux host REPL (30-second quickstart)

```sh
make                       # build liburbi.a + the urbi binary
echo "1 + 2" | ./build/host/urbi -i      # -> [..........] 3
./build/host/urbi -i                      # interactive REPL
```

## 2. Raspberry Pi Pico — `examples/pico/repl_demo`

```sh
make pico-repl-demo            # host urbi + hosted Cortex-M0+ archive + CMake build
# both built by the arm-none-eabi-gcc first on PATH; CMAKE=... picks the cmake
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

## 4. STM32F429I-DISC1 — `examples/stm32f4/mandelbrot`

```sh
make stm32f4-mandelbrot        # host urbi + freestanding Cortex-M4F archive + firmware
# -> examples/stm32f4/mandelbrot/build/mandelbrot.bin
STM32_Programmer_CLI -c port=SWD -w examples/stm32f4/mandelbrot/build/mandelbrot.bin 0x08000000 -rst
```

Connect the ST-Link VCP (115200 8N1): `picocom -b 115200 /dev/ttyACM0`. The
console prints a boot banner then `boot heap:` / `ready:` / `render:` lines;
the ILI9341 LCD renders the Mandelbrot set with progressive 32→1 tile
refinement; tilting the board pans the view (gyro) and the USER button zooms
2×. Full build, flash, wire and known-limits detail is in
[the example's own README](../../examples/stm32f4/mandelbrot/README.md).

## Notes

- Skip the ESP32-S3 step cleanly when `IDF_PATH` is unset (its build is
  environment-heavy); work through the other three regardless.
- A port that fails to build from a fresh tree is a release blocker (it would
  also block the Track A hardware regression).
