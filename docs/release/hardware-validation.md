# urbi-embedded hardware validation registry

> Canonical evidence registry for every target claimed as
> "hardware-supported" in README / ROADMAP / release-readiness.md.
>
> Each section captures one bring-up event. New bring-ups append a
> dated section. The most-recent date per target is the "Last verified"
> value carried in release-readiness.md.

## Raspberry Pi Pico (RP2040 / Cortex-M0+)

### 2026-05-24 — v0.9.4-pico-example

- **Board:** Raspberry Pi Pico (RP2040, dual Cortex-M0+, no FPU, no divide unit).
- **Toolchain:** xpack-arm-none-eabi-gcc 14.2.1.
- **SDK:** pico-sdk 2.2.0.
- **Firmware artifact:** `examples/pico/repl_demo/build/repl_demo.uf2` (size: see CHANGELOG v0.9.4 Footprint section).
- **Smoke steps:**
  1. Build firmware: `make cross-pico-repl`.
  2. Hold BOOTSEL on Pico, plug USB.
  3. Drag `repl_demo.uf2` to mounted `RPI-RP2` volume.
  4. Connect via `minicom -D /dev/ttyACM0 -b 115200`.
  5. Verify USB CDC REPL responds; verify BOOTSEL button press (QSPI_SS
     bit-bang + debounce) triggers C-side `urbi_register_watcher`
     callback that toggles GP25 LED via `gpio_xor_mask`.
- **Observed output:** Expected REPL banner; LED toggles on each BOOTSEL
  press after enumeration.
- **Known limitations:**
  - REPL session model too heavy for ~256 KB SRAM; per-session realm
    needs >50 KB on top of `stdlib_boot`'s 165 KB. Demo uses C-side
    `urbi_register_watcher` instead of scripted `whenever`.
  - `whenever (named_event)` body doesn't dispatch on cooperative builds
    (broken by construction per reactive audit F1; tracked in
    design-risks for a later wave).
- **Verifier:** aklofas.

## ESP32-S3-EYE

### 2026-05-16 — v0.7.2-esp32

- **Board:** ESP32-S3-EYE dev kit (Espressif, Xtensa LX7 dual-core, with
  hardware single-precision FPU).
- **Toolchain:** Espressif xtensa-esp-elf-gcc esp-15.2.0_20251204 (via
  ESP-IDF v6.0.1 bundled toolchain).
- **SDK:** ESP-IDF v6.0.1.
- **Firmware artifact:** `examples/esp32/eye_demo/build/eye_demo.bin`
  (~434 KB, within the 5 MB factory partition).
- **Smoke steps:**
  1. Build: `cd examples/esp32/eye_demo && idf.py build`.
  2. Flash: `idf.py -p /dev/ttyACM0 flash`.
  3. Monitor: `idf.py -p /dev/ttyACM0 monitor`.
  4. Verify camera-driven blob-tracking demo prints periodic detection
     logs; BOOT button cycles through RED → GREEN → BLUE tracking targets.
- **Observed output:** ~330-line urbiscript eye demo runs continuously;
  ST7789 LCD shows 240×240 crosshair overlay; OV2640 blob detection
  events visible in monitor output.
- **Known limitations:** None blocking at v0.7.2; 10 latent runtime bugs
  surfaced during bring-up were all fixed inline before ship.
- **Verifier:** aklofas.

## STM32F429I-DISC1

### 2026-05-17 — v0.8.2-stm32f4-mandelbrot

- **Board:** STM32F429I-DISC1 discovery kit (Cortex-M4F,
  2 MB flash, 192 KB main SRAM + 64 KB CCM, 240×320 ILI9341 LCD,
  L3GD20 gyro, hardware single-precision FPU).
- **Toolchain:** xpack-arm-none-eabi-gcc 14.2.1
  (`-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`).
- **SDK:** STM32CubeF4 v1.28.2 (HAL + CMSIS headers only; no CubeMX
  generated code).
- **Firmware artifact:** `examples/stm32f4/mandelbrot/build/mandelbrot.bin`.
- **Smoke steps:**
  1. Build: `cd examples/stm32f4/mandelbrot && make`.
  2. Flash via ST-LINK: `st-flash write build/mandelbrot.bin 0x8000000`.
  3. Reset; verify on-board ILI9341 LCD renders Mandelbrot set.
  4. Tilt the board to pan the view; press USER button to zoom 2× at
     current centre.
- **Observed output:** Progressive 32→1 pixel-tile Mandelbrot refinement
  renders on the LCD; serial console (ST-Link VCP, 115200 8N1) reports
  per-level timing.
- **Known limitations:**
  - `URBI_FLOAT_TYPE` link-time mismatch silently zeros every UVAL_FLOAT;
    the embedder application must pass `-DURBI_FLOAT_TYPE=4` to match
    the liburbi.a it links against. See `docs/embedding-guide.md` §FLOAT.
  - Button zoom is one-way (no zoom-out); gyro pan axes are rotated 90°
    from natural. Demo-only cosmetic issues.
- **Verifier:** aklofas.

## STM32H7

(planned; no hardware evidence yet)

## ESP32-C3

(planned; no hardware evidence yet)

## Adding a new bring-up

When verifying a new hardware target or re-verifying an existing one:

1. Append a new dated section under the appropriate target heading.
2. Capture: board model + revision, toolchain version, SDK version,
   firmware artifact path (and ideally artifact hash), exact smoke
   steps (build → flash → connect → verify), observed output, known
   limitations, verifier name.
3. Update `release-readiness.md`'s hardware-support table with the new
   "Last verified" date.
4. If the target was previously "planned", update README.md target
   table to "shipped".
