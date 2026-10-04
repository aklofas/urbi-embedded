# Clone → Build → Demo (v1.0)

This is the third-party "does it work from a fresh clone" path for each shipped
port. The goal is **≤ 5 minutes from clone to a running demo** on each
architecture. Both the host-side build of all firmware and hardware flashing
are manual and documented per-architecture below.

**Every embedded target below is parked until its own tag.** The core
re-foundation replaced the runtime each one was brought up against, and
none of the three component manifests / `examples/` workloads has been
rebuilt on it. Section 1 (the Linux host) builds and runs today; the
steps in sections 2-4 are kept as a record of what worked at each
port's own ship tag, not as commands that build against the tree as it
stands. The generic Cortex-M0+/M4F/M7/RISC-V archives build and are
footprint-measured on every push through the cross presets in
[the build system doc](../internals/build-system.md), but nothing wires
one into any of these three examples yet.

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
| Raspberry Pi Pico (RP2040) | `arm-none-eabi-gcc` (xpack 14.2.1) | pico-sdk 2.2.0 (`PICO_SDK_PATH`) | drag-drop `.uf2` (BOOTSEL) |
| ESP32-S3 | ESP-IDF v6.0.1 (`IDF_PATH`, `. $IDF_PATH/export.sh`) | bundled in IDF | `idf.py flash` |
| STM32F429I-DISC1 | `arm-none-eabi-gcc` (xpack 14.2.1) | STM32CubeF4 v1.28.2 headers (vendored) | `st-flash` (stlink) |

## 1. Linux host REPL (30-second quickstart)

```sh
make                       # build liburbi.a + the urbi binary
echo "1 + 2" | ./build/host/urbi -i      # -> [..........] 3
./build/host/urbi -i                      # interactive REPL
```

## 2. Raspberry Pi Pico — `examples/pico/repl_demo` (parked until its own tag)

`cross-pico-repl` and `test-cross-pico-repl-elf` are retired target
names from before the cross presets; the example's CMake also still
names two REPL transport files this core deleted, so it does not build
today. Kept as a record of the steps that worked at `v0.9.4-pico-example`:

```sh
export PICO_SDK_PATH=/path/to/pico-sdk    # or place it at ../tools/pico-sdk
make test-cross-pico-repl-elf
# -> examples/pico/repl_demo/build/repl_demo.uf2
```

(`make cross-pico-repl` alone built only the cross `liburbi.a`; the flashable
`.uf2` came from the pico-sdk CMake flow that `test-cross-pico-repl-elf` drove.
The `arm-cortex-m0plus` preset is the Cortex-M0+ archive's current name.)

Flash: hold **BOOTSEL**, plug USB, drag `repl_demo.uf2` onto the `RPI-RP2`
volume. Open the USB-CDC serial port (`minicom -D /dev/ttyACM0 -b 115200`);
you get a REPL banner. Type `1 + 2` → `3`. Pressing **BOOTSEL** toggles the
GP25 LED via a registered C watcher. (The full REPL is tight on RP2040 SRAM —
see the v0.9.4 notes; the C-side watcher path is the load-bearing demo.)

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
