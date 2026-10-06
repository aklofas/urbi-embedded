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

**The Raspberry Pi Pico and the STM32F429I-DISC1 are rebuilt on the
current core** — the Pico at `v0.16.1-pico` (builds in CI as
`cross-pico-repl`, hardware-validated) and the STM32F4 at
`v0.16.2-stm32f4` (builds in CI as `cross-stm32f4`, board log pending)
— and **ESP32-S3 stays parked until its own tag**: the core
re-foundation replaced the runtime it was brought up against, and it
has not been rebuilt on it: its component manifest, the `components/`
tree and the `examples/` workload are all in the tree and all out of
the build. The 32-bit memory figures are measured now, on a generic
Cortex-M4 under qemu's MPS2-AN386 model, through the `arm-cortex-m4f`
cross preset: a booted VM holds 48,980 bytes live (cap 49,152), an idle
sleeping strand costs 466 bytes, and the leak probes stay flat.
`make test-probes-32bit` regenerates them (it needs `arm-none-eabi-gcc`
with newlib and `qemu-system-arm`; CI runs it) — but that preset is
generic silicon, not any board below, so the number is real without
being a claim about one of these ports. Read the ESP32-S3 entry below
as a record of what it needed last time, not as a claim about today;
the Pico and STM32F4 entries describe what builds now.

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

- **Status:** rebuilt on the current core at `v0.16.2-stm32f4`; builds
  in CI (`cross-stm32f4`); board log pending, record in
  [`../release/hardware-validation.md`](../release/hardware-validation.md).
- **Toolchain and HAL:** xpack `arm-none-eabi-gcc` 14.2.1, STM32CubeF4
  v1.28.2 (HAL + CMSIS + the DISC1 BSP + the ili9341/l3gd20 component
  drivers; no CubeMX-generated code). The example links the
  `arm-cortex-m4f` preset's bytecode-only shape
  (`make cross-arm-cortex-m4f-bytecode-only`), 86,324 bytes
  text+data+bss (`size --totals`, measured 2026-10-05).
- **Shape of the example:** the component
  (`components/stm32f4-hal-baremetal/`) gives a 128 KB arena in
  internal SRAM through a bump-and-freelist allocator with its own
  requested-bytes counter (`port_alloc_live_bytes`); `tools/bake-header.sh`
  runs the host `urbi --dump-wire-format` on the workload at build
  time and turns the blob into the C header `urbi_load` loads at boot;
  the workload's main loop is a `detach`ed strand that renders,
  reports the render in a `render:` console line, and `waituntil`s the
  next trigger (a button press or a pan); `_Min_Stack_Size` in the
  linker script is 16 KB, a link-time check only — the stack really
  runs from `_estack` down to the end of `.bss`.
- **Numeric:** Float values are always double (f64); the Cortex-M4F FPU
  is single-precision, so double arithmetic is in software.
- **Idiosyncrasies kept from the first bring-up:**
  - No DCache on the F429 (Cortex-M4F has no D-cache controller).
    Bytecode reads from flash are deterministically slow but
    predictable; no need for cache-coherency dances.
  - 64 KB CCM is unreachable by DMA — useful for the GC arena, not for
    the LCD framebuffer.
  - The float layout is fixed (always f64), so there is no float-type
    build flag to keep in step between the application and liburbi.a.
  - The ILI9341's native surface is 320×240 landscape; the component's
    `port_lcd_fill_rect_native` rotates every rectangle 90° so the
    workload can address it as a 240×320 portrait surface
    (`port_lcd.c`).
  - TIM2 (the render tick) and the button's EXTI0 interrupt both call
    `urbi_inject_event`, whose ring has a single producer, so the two
    share one preemption priority — different priorities would let one
    preempt the other mid-inject and drop an event.
- **SDRAM option:** the VM's heap can move to the 8 MB SDRAM at
  `0xD0000000` by adding `-DURBI_HEAP_EXTERNAL_ADDR=0xD0080000UL
  -DURBI_HEAP_BYTES=1048576UL` to the example's `DEFS`; not the
  default, and slower than internal SRAM.
- **Flashing:** `STM32_Programmer_CLI -c port=SWD -w
  build/mandelbrot.bin 0x08000000 -rst` first (`make -C
  examples/stm32f4/mandelbrot flash`); `st-flash --reset write
  build/mandelbrot.bin 0x08000000` second (`make -C
  examples/stm32f4/mandelbrot flash-stlink`).

## Raspberry Pi Pico (RP2040, Cortex-M0+)

- **Status:** rebuilt on the current core at `v0.16.1-pico`; builds in
  CI (`cross-pico-repl`); hardware-validated 2026-10-05 (boot heap
  48,980 B on the part, a session reopening with zero drift, 150 s idle
  flat), log in
  [`../release/hardware-validation.md`](../release/hardware-validation.md).
- **Toolchain and SDK:** xpack `arm-none-eabi-gcc` 14.2.1, pico-sdk
  2.2.0 (TinyUSB 0.18), the `arm-cortex-m0plus` preset in its hosted
  shape (`make cross-arm-cortex-m0plus-hosted`, 146,823 B
  text+data+bss at this tag). Soft-float through libgcc; floats are
  always `double`.
- **Shape of the example:** CMake imports the archive; `bake.cmake`
  runs the host `urbi --dump-wire-format` on `repl_demo.u`; the two
  `UTransport` adapters live in the example
  (`main/transport_usb_cdc.c`, `main/transport_uart.c`); verbs sit on
  `Lobby`, events on `Object` via `urbi_event_value`; a 64 KB stack at
  the top of SRAM comes from `memmap_repl_demo.ld`, with the SDK's MPU
  stack guard at its bottom (`PICO_USE_STACK_GUARDS`); the heap budget
  is the linker's heap minus a 16 KB session reserve; compile budget
  depth 12 / nodes 1,000 / source 1 KiB, sized to measured stack use
  (16,880 B at `1+1`, 2,976 B worst per expression-nesting level).
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
  `UReplConfig`); the parser's depth budget counts expression nesting
  only, so deep statement or brace nesting inside the 1 KiB source cap
  can still overrun the stack.  The 32-byte guard at the stack bottom
  turns most overruns into a hard fault and the LED error pattern; a
  frame larger than 32 B can step over it, so a deep enough line can
  still reach the heap until the parser counts statement nesting.
  UART0 input arrives through an RX-interrupt ring, and the main loop
  does not sleep while UART0 output is part-way out.
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
