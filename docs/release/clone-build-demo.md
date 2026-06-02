# Clone → Build → Demo (v1.0)

This is the third-party "does it work from a fresh clone" path for each shipped
port. The goal is **≤ 5 minutes from clone to a running demo** on each
architecture. The host-side build of all firmware is verified automatically by
`tests/scripts/clone-build-demo.sh` (run it via `make clone-build-demo-check`);
hardware flashing is manual and documented per-architecture below.

```sh
git clone <repo-url> urbi-embedded
cd urbi-embedded
make clone-build-demo-check     # builds: linux REPL + pico + stm32f4 (+ esp32 if IDF_PATH set)
```

The harness does a `make clean` first, so it proves a **pristine** tree builds
(no stale `build/` artifacts).

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

## 2. Raspberry Pi Pico — `examples/pico/repl_demo`

```sh
make cross-pico-repl
# -> examples/pico/repl_demo/build/repl_demo.uf2
```

Flash: hold **BOOTSEL**, plug USB, drag `repl_demo.uf2` onto the `RPI-RP2`
volume. Open the USB-CDC serial port (`minicom -D /dev/ttyACM0 -b 115200`);
you get a REPL banner. Type `1 + 2` → `3`. Pressing **BOOTSEL** toggles the
GP25 LED via a registered C watcher. (The full REPL is tight on RP2040 SRAM —
see the v0.9.4 notes; the C-side watcher path is the load-bearing demo.)

## 3. ESP32-S3 — `examples/esp32/eye_demo`

```sh
. $IDF_PATH/export.sh
cd examples/esp32/eye_demo && idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The ~330-line eye demo runs a blob-detection loop; the BOOT button cycles the
RGB LED (RED→GREEN→BLUE) tracking targets and the LCD shows a crosshair overlay.

## 4. STM32F429I-DISC1 — `examples/stm32f4/mandelbrot`

```sh
cd examples/stm32f4/mandelbrot && make
# -> build/mandelbrot.bin
st-flash write build/mandelbrot.bin 0x8000000
```

Reset; connect the ST-Link VCP (115200 8N1). The ILI9341 LCD renders the
Mandelbrot set with progressive 32→1 tile refinement; tilting the board pans the
view (gyro) and the USER button zooms 2×. This port runs `URBI_FLOAT_TYPE=4`
(f32) — the embedder MUST keep the float-type macro consistent or every
`UVAL_FLOAT` silently truncates to 0.0 (link-time-guarded since v0.8.2).

## Notes

- `make clone-build-demo-check` skips ESP32-S3 cleanly when `IDF_PATH` is unset
  (its build is environment-heavy); the other three are always exercised.
- A non-zero exit means a port stopped building from a fresh tree — that is a
  release blocker (it would also block the Track A hardware regression).
