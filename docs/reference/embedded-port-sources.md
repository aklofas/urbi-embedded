# Embedded Port Sources

Upstream vendor SDKs that urbi-embedded's port layer consumes, pinned
to specific tags + commit SHAs so a fresh clone reproduces the exact
silicon-side toolchain in CI and in published examples. None of these
SDKs are vendored into the urbi-embedded repository; they live as peer
checkouts alongside the repository and are picked up by per-target
Makefiles + CMake glue.

For per-target build recipes (`cross-arm`, `cross-pico`, `cross-esp32s3`,
...), see [internals/build-system.md](../internals/build-system.md).
For per-silicon footprint + idiosyncrasy notes, see
[internals/ports.md](../internals/ports.md).

---

## ESP-IDF (Espressif IoT Development Framework)

- **Repository:** `https://github.com/espressif/esp-idf.git`
- **Pinned tag:** `v6.0.1`
- **Local checkout path:** `tools/esp-idf/` (workspace root).
- **Toolchain:** Bundled `xtensa-esp32s3-elf-gcc` 13.x via
  `tools/esp-idf-tools/`.
- **First used by:** `v0.7.2-esp32` (2026-05-16).
- **Notes:**
  - `urbi` ESP-IDF managed component lives at `components/urbi/`; the
    `urbi_aux` component is separate so applications can omit it.
  - The newlib shipped in ESP-IDF is **hosted**, not freestanding —
    the `urbi` component must NOT pass `-ffreestanding`. `urbi_aux`
    keeps the flag (the host-tool generator is freestanding-clean).

## STM32CubeF4 (STMicroelectronics)

- **Repository:** `https://github.com/STMicroelectronics/STM32CubeF4.git`
- **Pinned tag:** `v1.28.2`
- **Local checkout path:** `tools/stm32cube-f4/` (workspace root).
- **Toolchain:** Distribution-packaged `arm-none-eabi-gcc` 12+.
- **First used by:** `v0.8.2-stm32f4-mandelbrot` (2026-05-17).
- **Notes:**
  - Used only for the HAL / CMSIS headers + the STM32F429-DISC1 board
    BSP. The Mandelbrot demo's reset + clock-init is hand-written and
    does NOT pull in CubeMX-generated code.
  - The HAL is not linked into liburbi.a itself — it is consumed only
    by the embedder's `examples/stm32f4-disc/` application.

## pico-sdk (Raspberry Pi RP-series)

- **Repository:** `https://github.com/raspberrypi/pico-sdk.git`
- **Pinned tag:** `2.2.0`
- **Pinned commit:** `a1438dff1d38bd9c65dbd693f0e5db4b9ae91779`
- **Local checkout path:** `tools/pico-sdk/` (workspace root).
- **Toolchain:** Distribution-packaged `arm-none-eabi-gcc` 12+; the
  pico-sdk's CMake glue auto-selects `-mcpu=cortex-m0plus` etc. based
  on `PICO_BOARD`.
- **First used by:** `v0.9.4-pico-example` (2026-05-20).
- **Submodules required:** `btstack`, `cyw43-driver`, `lwip`,
  `mbedtls` (+ `mbedtls/framework`), `tinyusb` — checkout with
  `git submodule update --init --recursive`.
- **Submodule pins** (informational; carried by the pico-sdk tag,
  recorded here so a partial clone can be reproduced manually):
  - `btstack` — `501e6d2b`
  - `cyw43-driver` — `dd756822`
  - `lwip` — `77dcd25a`
  - `mbedtls` — `107ea89d` (+ `mbedtls/framework` `94599c0e`)
  - `tinyusb` — `86ad6e56`
- **Notes:**
  - `examples/pico/repl_demo/` consumes pico-sdk through the standard
    `pico_sdk_import.cmake` shim vendored next to the example's
    `CMakeLists.txt`.
  - liburbi.a is exposed to the example via `IMPORTED STATIC` from
    `build/arm-cortex-m0plus/liburbi.a`; the pico-sdk handles every
    other linker-script + reset-vector concern.
  - TinyUSB CDC drives the primary REPL transport
    (`UREPL_USB_CDC_PICO_TRANSPORT`); see
    [`../embedding-guide.md`](../embedding-guide.md) §12.

## See also

- [internals/build-system.md](../internals/build-system.md) — per-target
  Make recipes.
- [internals/ports.md](../internals/ports.md) — per-silicon footprint +
  idiosyncrasy notes.
- [cross-toolchain-setup.md](../cross-toolchain-setup.md) — installing
  cross toolchains; the probe-compile model used by `releasetest`.
