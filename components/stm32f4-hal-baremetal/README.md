# stm32f4-hal-baremetal

Bare-metal STM32F4 port using STMicroelectronics Cube HAL/BSP, no RTOS.
Targets STM32F429I-DISC1 specifically; portable to other F4 boards by
adapting the BSP layer.

## Build

This component is consumed by example projects under `examples/stm32f4/`.
See the example Makefile for the full `arm-none-eabi-gcc` invocation
(toolchain flags, linker script, BSP paths into `tools/stm32cube-f4/`).

For cross-compile sanity check of urbi-core against the F4 target:

```sh
make cross-stm32f4              # full build
make cross-stm32f4-bytecode-only # bytecode-only freestanding gate
```

## Public API

See `include/port_stm32f4.h` for the wrapper functions embedders pass into
the urbi register hooks.

## Memory layout (F429ZIT6)

| Region | Address    | Size   | Purpose                           |
|--------|------------|--------|-----------------------------------|
| FLASH  | 0x08000000 | 2 MB   | code + .rodata + baked bytecode   |
| SRAM1  | 0x20000000 | 112 KB | .data + .bss + stack + urbi heap  |
| CCM    | 0x10000000 | 64 KB  | reserved                          |
| SDRAM  | 0xD0000000 | 8 MB   | framebuffer + spare               |

`urbi_heap` carved at `URBI_HEAP_BYTES` (default 80 KB) from SRAM1.
