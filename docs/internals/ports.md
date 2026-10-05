# Embedded ports

This document is the running index of the embedded targets the
codebase has been brought up on. Each entry records the toolchain,
text/SRAM footprint, numeric configuration, REPL-transport binding,
build system, and the per-silicon idiosyncrasies that bit during
bring-up. Cross-toolchain setup (probe-compile, sysroot pitfalls) is
in [`../cross-toolchain-setup.md`](../cross-toolchain-setup.md); per-
target Make recipes are in [`build-system.md`](./build-system.md).

The host build is not a port — `make` (POSIX glibc) is the canonical
development target. Ports below cover bare-metal + RTOS silicon.

**The Raspberry Pi Pico is rebuilt on the current core at
`v0.16.1-pico`** and builds in CI (`cross-pico-repl`); **ESP32-S3 and
STM32F4 stay parked until their own tags** — the core re-foundation
replaced the runtime they were brought up against, and neither has been
rebuilt on it: the component manifests, the `components/` trees and the
`examples/` workloads are all in the tree and all out of the build.
The 32-bit memory figures are measured now, on a generic Cortex-M4
under qemu's MPS2-AN386 model, through the `arm-cortex-m4f` cross
preset: a booted VM holds 48,980 bytes live (cap 49,152), an idle
sleeping strand costs 466 bytes, and the leak probes stay flat.
`make test-probes-32bit` regenerates them (it needs `arm-none-eabi-gcc`
with newlib and `qemu-system-arm`; CI runs it) — but that preset is
generic silicon, not any board below, so the number is real without
being a claim about one of these ports. Read the ESP32-S3 and STM32F4
entries below as a record of what each target needed last time, not as
a claim about today; the Pico entry describes what builds now.

## ESP32-S3 (Espressif, Xtensa LX7)

- **Status:** Shipped at `v0.7.2-esp32` (2026-05-16); parked until its
  own tag. Validated on ESP32-S3-EYE silicon with the `eye_demo`
  workload (LED + on-die temperature + AOV blob tracking).
- **Toolchain:** ESP-IDF v6.0.1 (`xtensa-esp32s3-elf-gcc`); hosted
  newlib (do NOT pass `-ffreestanding` to the `urbi` component;
  `urbi_aux` separately).
- **Footprint:** ~120 KB liburbi.a text (cap revised from 105 KB during
  bring-up). PSRAM available on the EYE variant.
- **Numeric:** Float values are always double (f64); the Xtensa LX7's
  hardware FPU is single-precision, so double arithmetic is in software.
- **REPL transports:** UART0 console + USB CDC (via TinyUSB ESP-IDF
  managed component). `UREPL_ESP_IDF_UART_TRANSPORT` is the primary;
  cooperative drive via `urbi_repl_serve_step` from v0.9.4 onwards.
- **Build system:** ESP-IDF CMake managed components at
  `components/urbi/` + `components/urbi_aux/`; consumed by the
  application's `idf.py build`. Component manifests pin the upstream
  liburbi.a layout.
- **Idiosyncrasies:**
  - ESP-IDF's newlib is hosted, so the `urbi` component does NOT pass
    `-ffreestanding` (latent landmine fixed during v0.7.2 ship).
  - PSRAM read/write latency is ~5× internal SRAM — keep the bytecode
    and interned-string pool in internal SRAM; large blob buffers
    can spill to PSRAM.
  - `vm->last_recv` was retired pre-ship (S42); methods receive the
    receiver through `OP_SELF` instead. Wire format bumped v1.5→v1.6.
  - Eye demo as a bug-detector caught 10 latent runtime issues during
    bring-up (waituntil cascade-wake, body-strand module==NULL, &
    chunk-top driver gap, brace-block bug, etc.).

## STM32F429I-DISC1 (STMicroelectronics, Cortex-M4F)

- **Status:** Shipped at `v0.8.2-stm32f4-mandelbrot` (2026-05-17);
  parked until its own tag. First non-RTOS port; bare-metal
  `Reset_Handler` + custom linker script. Validated with a Mandelbrot
  rendering workload on the 240×320 onboard ILI9341 LCD.
- **Toolchain:** `arm-none-eabi-gcc` 12+; ARMv7E-M Thumb-2 with
  hardware FPU (`-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`).
- **Footprint:** ~110 KB liburbi.a text; ~140 KB bytecode-only.
  STM32F429 ships with 2 MB flash + 256 KB SRAM (192 KB main + 64 KB
  CCM).
- **Numeric:** Float values are always double (f64); the Cortex-M4F FPU
  is single-precision, so double arithmetic is in software.
- **REPL transports:** None at v0.8.2 (pre-M8). Embedder drives
  `urbi_step` from the main loop; output flows via the ILI9341.
- **Build system:** Plain `arm-none-eabi-gcc` Makefile under
  `examples/stm32f4-disc/`; no STM32CubeMX / CubeIDE / CMSIS layer
  beyond hand-written reset + clock init.
- **Idiosyncrasies:**
  - The float layout is fixed (always f64), so there is no float-type
    build flag to keep in step between the application and liburbi.a.
  - No DCache on the F429 (Cortex-M4F has no D-cache controller).
    Bytecode reads from flash are deterministically slow but
    predictable; no need for cache-coherency dances.
  - 64 KB CCM is unreachable by DMA — useful for the GC arena, not for
    the LCD framebuffer.

## Raspberry Pi Pico (RP2040, Cortex-M0+)

- **Status:** rebuilt on the current core at `v0.16.1-pico`; builds in
  CI (`cross-pico-repl`); the board log that closes the tag is
  recorded in
  [`../release/hardware-validation.md`](../release/hardware-validation.md)
  once the owner runs it.
- **Toolchain and SDK:** xpack `arm-none-eabi-gcc` 14.2.1, pico-sdk
  2.2.0 (TinyUSB 0.18), the `arm-cortex-m0plus` preset in its hosted
  shape (`make cross-arm-cortex-m0plus-hosted`, 146,760 B
  text+data+bss at this tag). Soft-float through libgcc; floats are
  always `double`.
- **Shape of the example:** CMake imports the archive; `bake.cmake`
  runs the host `urbi --dump-wire-format` on `repl_demo.u`; the two
  `UTransport` adapters live in the example
  (`main/transport_usb_cdc.c`, `main/transport_uart.c`); verbs sit on
  `Lobby`, events on `Object` via `urbi_event_value`; a 32 KB stack at
  the top of SRAM comes from `memmap_repl_demo.ld`; the heap budget is
  the linker's heap minus a 16 KB session reserve; compile budget
  depth 24 / nodes 2,000 / source 4 KB.
- **Idiosyncrasies:**
  - **BOOTSEL button** is the only onboard button; reading it requires
    the QSPI_SS bit-bang trick with interrupts off for the sample.
  - **On-die temperature sensor** is on ADC4.
  - **TinyUSB CDC** is single-host: one CDC interface, so one USB
    session at a time; UART0 carries the second channel.
  - Two-core (Cortex-M0+ × 2); core1 stays dormant.
  - No integer-divide hardware: every `/` and `%` goes through libgcc
    soft-divide helpers.
- **Known limits:** the eval service's framing cap is fixed at 1 MiB
  (`UREPL_MAX_LINE` in `src/repl/urepl_ndjson.h`, not exposed through
  `UReplConfig`); UART input latency runs up to the 100 ms tick when
  the main loop is otherwise idle.
- **Pico SDK pin:** `2.2.0` at commit
  `a1438dff1d38bd9c65dbd693f0e5db4b9ae91779` (recorded in
  [`../reference/embedded-port-sources.md`](../reference/embedded-port-sources.md)).

## See also

- [`build-system.md`](./build-system.md) — the cross presets
  (`arm-cortex-m0plus`, `arm-cortex-m4f`, `arm-cortex-m7`, `riscv32`)
  and their `cross-<preset>` / `cross-<preset>-bytecode-only` /
  `cross-<preset>-hosted` Make recipes.
- [`../cross-toolchain-setup.md`](../cross-toolchain-setup.md) —
  installing cross toolchains; the probe-compile model used by
  `releasetest`.
- [`../reference/embedded-port-sources.md`](../reference/embedded-port-sources.md)
  — upstream-vendor SDK + tag pins consumed by these ports.
