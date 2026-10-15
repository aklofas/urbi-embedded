# urbi-embedded hardware validation registry

> Canonical evidence registry for every target claimed as
> "hardware-supported" in README / ROADMAP / release-readiness.md.
>
> Each section captures one bring-up event. New bring-ups append a
> dated section. The most-recent date per target is the "Last verified"
> value carried in release-readiness.md.

## Raspberry Pi Pico (RP2040 / Cortex-M0+)

### 2026-10-05 — v0.16.1-pico

- **Board:** Raspberry Pi Pico (RP2040, dual Cortex-M0+, no FPU, no divide unit).
- **Toolchain:** the bench machine's STM32CubeCLT `arm-none-eabi-gcc` 13.3.1 for both the archive and the firmware (CI builds the same sources with xpack 14.2.1).
- **SDK:** pico-sdk 2.2.0.
- **Firmware artifact:** `examples/pico/repl_demo/build/repl_demo.uf2` built by `make pico-repl-demo` from branch head `24d71619`; `repl_demo.elf` text 234,640 / data 0 / bss 4,012 B (552,960 B `.uf2` and 280,384 B text with xpack).
- **Smoke steps:**
  1. Boot heap: the `boot heap:` line shows `alloc live` under 49,152 B.
  2. Three evals: `echo("hi")` prints and returns `nil`; `1+1` returns `2`; `temp_celsius()` returns a float.
  3. Watcher from the session: after `boot.stop()`, BOOTSEL no longer toggles the LED; after `at (pressed?) led_toggle()`, BOOTSEL toggles it on each press.
  4. Periodic print: a one-second `every` tag prints a temperature once a second, then stops on `t.stop()`.
  5. Session reopen: the `session open:` line's `alloc live` is within 1,024 B across a close and reopen.
  6. Idle: two `idle:` lines 30 s apart show the same `alloc live`.
- **Observed output** (picocom on `/dev/ttyACM0`, condensed; the board's own lines verbatim):

  ```text
  === urbi 0.16.1-pico on Raspberry Pi Pico ===
  [1] urbi_open... ok
  boot heap: alloc live 48980 B, gc live 27224 B, heap break 65780 B, budget 170100 B, cycles 2
  [2] fixtures... ok
  [3] workload... ok
  [4] eval service... ok
  [5] tick... ok
  ready: alloc live 51899 B, gc live 28643 B, heap break 68468 B, budget 170100 B, cycles 3
  {"kind":"output","channel":"console","msg":"session open: alloc live 52131 B, gc live 28875 B, heap break 68468 B, budget 170100 B, cycles 4\r\n"}
  {"id":1,"kind":"output","channel":"clog","msg":"[00035952] *** hi\n"}
  {"id":1,"kind":"result","value":"nil"}
  {"id":2,"kind":"result","value":"2"}
  {"id":3,"kind":"result","value":"22.457083454000"}
  {"id":4,"kind":"result","value":"nil"}            boot.stop(); BOOTSEL no longer toggles the LED
  {"id":5,"kind":"result","value":"nil"}            at (pressed?) led_toggle(); BOOTSEL toggles it on each press
  {"id":6,"kind":"result","value":"nil"}
  {"kind":"output","channel":"clog","msg":"[00097306] *** 22.457083454000\n"}
  {"kind":"output","channel":"clog","msg":"[00098305] *** 22.457083454000\n"}   ... once a second, 14 lines ...
  {"id":7,"kind":"result","value":"nil"}            t.stop(); no further lines
  {"kind":"output","channel":"console","msg":"idle: alloc live 51578 B, ..."}
  (picocom closed and reopened)
  {"kind":"output","channel":"console","msg":"session open: alloc live 50411 B, gc live 28655 B, heap break 94836 B, budget 170100 B, cycles 13\r\n"}
  {"kind":"output","channel":"console","msg":"idle: alloc live 50411 B, ... cycles 14\r\n"}   ... five idle lines, 30 s apart, all 50411 ...
  (picocom closed and reopened again)
  {"kind":"output","channel":"console","msg":"session open: alloc live 50411 B, gc live 28655 B, heap break 94836 B, budget 170100 B, cycles 20\r\n"}
  ```

  Items 1 to 6 all pass: boot heap 48,980 B (identical to the qemu Cortex-M4 probe); the three evals answer as listed (the Float prints with twelve fixed decimals because pico-sdk's printf renders `%g` that way, unlike the host); the LED follows the session's watcher once the boot tag is stopped; the periodic prints at 1,000 ms spacing and stops; a close and reopen lands on the same 50,411 B (the first session's 52,131 B included the two boot watchers that `boot.stop()` freed); five idle readings over 150 s show no growth, with newlib's high-water mark flat at 94,836 B.
- **Verifier:** aklofas.

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

### 2026-10-06 — v0.16.2-stm32f4

- **Board:** STM32F429I-DISC1 discovery kit (Cortex-M4F, 2 MB flash,
  192 KB main SRAM + 64 KB CCM, 240×320 ILI9341 LCD, L3GD20 gyro,
  hardware single-precision FPU).
- **Toolchain:** STM32CubeCLT 13.3.1 `arm-none-eabi-gcc` on the bench
  (xpack 14.2.1 in CI; the two differ in code size, see below).
- **SDK:** STM32CubeF4 v1.28.2 (HAL + CMSIS + the DISC1 BSP + the
  ili9341/l3gd20 component drivers; no CubeMX-generated code).
- **Firmware artifact:** `examples/stm32f4/mandelbrot/build/mandelbrot.bin`
  built by `make stm32f4-mandelbrot` from branch head `0107f34a`;
  `mandelbrot.elf` text 119,112 / data 164 / bss 148,800 B on the bench
  (119,608 / 164 / 148,800 B and a 119,776 B `.bin` with xpack).
- **Flashing:** `st-flash --reset write build/mandelbrot.bin 0x08000000`
  (`make -C examples/stm32f4/mandelbrot flash-stlink`). STM32CubeProgrammer
  2.21 refused this board's ST-LINK firmware (V2J25M14) and would need the
  ST-LINK upgraded first; `st-flash` from the open-source stlink tools
  accepted it as-is.
- **Smoke steps:**
  1. Boot heap: the `boot heap:` line shows `alloc live` under
     49,152 B.
  2. First render: the LCD fills in coarse tiles that refine down to
     single pixels, and the first `render:` line prints its time and
     live bytes.
  3. Re-render: each USER press prints a new `render:` line whose
     `alloc live` is within 1,024 B of the first one's.
  4. Ten presses: after ten presses there is no `OutOfMemoryError`
     line and the board has not reset.
- **Observed output** (picocom on `/dev/ttyACM0`, four sittings on
  2026-10-06; figures from the board's own lines):

  | Reading | Value |
  |---|---|
  | `boot heap: alloc live` | 48,938 B (cap 49,152), identical across every reset |
  | `ready: alloc live` | 64,782 B |
  | full render at the start view | 76,294 ms; `after render: alloc live` 68,782 B |
  | deep zoom (main cardioid, every pixel at max iterations) | 144,837 ms |
  | ~100 re-renders (10+ USER presses, gyro pans, mid-render aborts) | `alloc live` 68,014–68,910 B, worst 896 B over the first render |
  | `heap top` after the first render | 103,968 B of the 131,072 B arena (budget 98,304 B) |
  | `heap top` after ~100 renders | 105,024 B (two 528 B steps) |
  | `OutOfMemoryError` lines / resets | none |

  Items 1 to 4 all pass. The first three sittings ran with the whole
  arena declared as the heap budget: `alloc live` behaved the same, but
  `heap top` reached 130,608 B of the 131,072 B arena, because the
  collector paces only the bytes it tracks while the allocator's block
  headers and the VM's untracked memory share the arena. Commit
  `0107f34a` sets the budget to three quarters of the arena, and the
  fourth sitting above is on that build. The tilt-driven ~40 ms
  re-renders in the log are the gyro pan redrawing a few tiles, not a
  fault; a press mid-render aborts it, so such a press prints two
  `render:` lines. The slow `heap top` creep is filed as a known limit
  (fragmentation; `alloc live` is flat). RESET reboots to the same
  `boot heap:` figure.
- **Verifier:** aklofas.

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
  - Validated when the port carried a per-target float width; the float
    layout has since been fixed at f64 on every target, so that build
    flag no longer exists and the run is due a re-validation.
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
