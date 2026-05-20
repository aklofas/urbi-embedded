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

## ESP32-S3 (Espressif, Xtensa LX7)

- **Status:** Shipped at `v0.7.2-esp32` (2026-05-16). Validated on
  ESP32-S3-EYE silicon with the `eye_demo` workload (LED + on-die
  temperature + AOV blob tracking).
- **Toolchain:** ESP-IDF v6.0.1 (`xtensa-esp32s3-elf-gcc`); hosted
  newlib (do NOT pass `-ffreestanding` to the `urbi` component;
  `urbi_aux` separately).
- **Footprint:** ~120 KB liburbi.a text (revised cap from 105 KB during
  bring-up; see REVIVAL §S32/§S33). PSRAM available on the EYE variant.
- **Numeric:** URBI_FLOAT_TYPE=4 (single precision); the Xtensa LX7
  has hardware single-precision FPU.
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

- **Status:** Shipped at `v0.8.2-stm32f4-mandelbrot` (2026-05-17).
  First non-RTOS port; bare-metal `Reset_Handler` + custom linker
  script. Validated with a Mandelbrot rendering workload on the
  240×320 onboard ILI9341 LCD.
- **Toolchain:** `arm-none-eabi-gcc` 12+; ARMv7E-M Thumb-2 with
  hardware FPU (`-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`).
- **Footprint:** ~110 KB liburbi.a text; ~140 KB bytecode-only.
  STM32F429 ships with 2 MB flash + 256 KB SRAM (192 KB main + 64 KB
  CCM).
- **Numeric:** URBI_FLOAT_TYPE=4 (single precision); hardware FPU.
- **REPL transports:** None at v0.8.2 (pre-M8). Embedder drives
  `urbi_step` from the main loop; output flows via the ILI9341.
- **Build system:** Plain `arm-none-eabi-gcc` Makefile under
  `examples/stm32f4-disc/`; no STM32CubeMX / CubeIDE / CMSIS layer
  beyond hand-written reset + clock init.
- **Idiosyncrasies:**
  - **`URBI_FLOAT_TYPE` link-time mismatch silently zeros every
    `UVAL_FLOAT` to 0.0** — discovered during bring-up. The embedder
    application must pass `-DURBI_FLOAT_TYPE=4` matching the liburbi.a
    it links against. v1.0-rc weak-symbol guard sketched; filed.
  - No DCache on the F429 (Cortex-M4F has no D-cache controller).
    Bytecode reads from flash are deterministically slow but
    predictable; no need for cache-coherency dances.
  - 64 KB CCM is unreachable by DMA — useful for the GC arena, not for
    the LCD framebuffer.

## Raspberry Pi Pico (RP2040, Cortex-M0+)

- **Status:** Designed at `v0.9.4-pico-example` (2026-05-20). Hardware
  bring-up pending Phase 8 of the workspace-root
  `docs/superpowers/plans/2026-05-19-v0.9.4-pico-example.md`
  execution plan (workspace-root tree, not tracked in this repo).
- **Toolchain:** `arm-none-eabi-gcc` 12+; ARMv6-M Thumb-2 subset
  (`-mcpu=cortex-m0plus`); soft-float + libgcc helpers
  (`__divsf3`, `__mulsf3`, `__addsf3`, `__udivdi3`, `__divdi3`,
  `__floatsidf`, `__fixdfdi`, etc. — see
  `tests/cross/pico_freestanding_symbols.golden`).
- **Footprint:** *TBD — calibrated at first hardware bring-up; see
  Phase 8 of the plan. Initial estimate: 100–130 KB liburbi.a text;
  200–250 KB repl_demo.uf2.*
- **Numeric:** URBI_FLOAT_TYPE=4 (single precision); the M0+ has no
  FPU so all float arithmetic goes through libgcc soft-float helpers.
- **REPL transports:** USB CDC (primary, via TinyUSB) on the native
  USB pins, UART0 (secondary, GP0/GP1). Both have
  `pollable_fd_fn == NULL` and drive `urbi_repl_serve_step`
  cooperatively from the main loop.
- **Build system:** CMake-native via pico-sdk
  (`tools/pico-sdk/external/pico_sdk_import.cmake`); consumes
  liburbi.a as an `IMPORTED STATIC` library from
  `build/arm-cortex-m0plus/`. No autotools, no idf.py.
- **Idiosyncrasies:**
  - **No integer divide hardware.** Every `/` and `%` on `int32_t` /
    `uint64_t` goes through libgcc soft-divide helpers; the freestanding
    golden symbol list pins this expectation.
  - **BOOTSEL button** is the only onboard button and reading it
    requires QSPI_SS bit-bang with a ~30 µs interrupt-off window
    (`save_and_disable_interrupts` / `restore_interrupts` around the
    sample). See `bsp/button.c` in the repl_demo example.
  - **On-die temperature sensor** is on ADC4; raw value passes through
    the SDK's calibration formula `27 - (V_be - 0.706) / 0.001721`
    with ~±5°C accuracy.
  - **TinyUSB CDC** is single-host: only one CDC interface, only one
    attached host at a time. Multi-client REPL on USB is not
    physically possible — pair with UART0 for the second channel.
  - Two-core (Cortex-M0+ × 2); core1 is dormant unless the embedder
    explicitly starts it. liburbi.a is single-VM-per-thread and runs
    on core0 only.
- **Pico SDK pin:** `2.2.0` at commit
  `a1438dff1d38bd9c65dbd693f0e5db4b9ae91779` (recorded in
  [`../reference/embedded-port-sources.md`](../reference/embedded-port-sources.md)).

## See also

- [`build-system.md`](./build-system.md) — per-target Make recipes
  (`cross-arm`, `cross-riscv`, `cross-esp32s3`, `cross-pico`, ...).
- [`../cross-toolchain-setup.md`](../cross-toolchain-setup.md) —
  installing cross toolchains; the probe-compile model used by
  `releasetest`.
- [`../reference/embedded-port-sources.md`](../reference/embedded-port-sources.md)
  — upstream-vendor SDK + tag pins consumed by these ports.
