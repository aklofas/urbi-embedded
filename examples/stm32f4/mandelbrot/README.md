# STM32F429I-DISC1 Mandelbrot demo

## What this is

This example renders the Mandelbrot set on the STM32F429I-DISC1's LCD from urbiscript, refining it coarse to fine, with the VM running out of a 128 KB heap in internal SRAM.
The board exposes six verbs (`lcd_fill_rect(x, y, w, h, rgb565)`, `gyro_x()`, `gyro_y()`, `gyro_z()`, `render_begin()`, `render_end()`) and two events (`button_press` on each USER press and `gyro_tick` every 50 ms).
The workload is [`mandelbrot.u`](mandelbrot.u); it is compiled on the host at build time, baked into the firmware as bytecode, and loaded into the main realm at boot.

## Build

Prerequisites:

- An `arm-none-eabi-gcc` on `PATH`.
- STM32CubeF4 v1.28.2, either at `HAL_ROOT` or at `../tools/stm32cube-f4` beside the repository.

From the repository root:

```sh
make stm32f4-mandelbrot
```

That one command builds the host `urbi` (which compiles `mandelbrot.u` to bytecode), the freestanding bytecode-only Cortex-M4F `liburbi.a`, and then this example through its own Makefile.
It ends by printing the firmware's `size` line.

The archive and the firmware are both built by the first `arm-none-eabi-gcc` on `PATH`, so they always come from one toolchain.
Building with a different compiler behind that name rebuilds the archive.
The sizes quoted in this file are from xpack 14.2.1; other toolchains build and run too, with a different image size.

Outputs, under `examples/stm32f4/mandelbrot/`:

- `build/mandelbrot.elf`, the ELF with symbols, for a debug probe.
- `build/mandelbrot.bin`, the image to flash at 0x08000000.

## Flash

With the board on its ST-Link USB port:

```sh
STM32_Programmer_CLI -c port=SWD -w build/mandelbrot.bin 0x08000000 -rst
```

`make -C examples/stm32f4/mandelbrot flash` runs the same command from the repository root.
`make -C examples/stm32f4/mandelbrot flash-stlink` flashes through `st-flash` instead.

## Connect

USART1 is routed to the ST-Link's virtual COM port, at 115200 8N1:

```sh
picocom -b 115200 /dev/ttyACM0
```

Leave picocom with C-a C-x.
Press the black RESET button to see the whole boot.

- The blue **USER** button zooms in 2x at the centre of the view and starts a new render.
- **Tilting** the board pans the view once the turn rate passes a deadzone; the bar along the top edge goes from green through yellow and orange to red as the board turns faster.

## Console lines

The shim prints a banner, one line per boot stage (`[1] urbi_open... ok` through `[4] tick... ok`), then the heap lines:

```text
boot heap: alloc live <n> B, gc live <n> B, heap top <n> B, budget <n> B
ready: alloc live <n> B, gc live <n> B, heap top <n> B, budget <n> B
render: <ms> ms, after render: alloc live <n> B, gc live <n> B, heap top <n> B, budget <n> B
```

Each heap figure is read right after a full collection.

- `alloc live` is the bytes the VM has requested from its allocator and not yet freed. This is the figure the 48 KB boot target caps.
- `gc live` is the collector's own view of what it holds live.
- `heap top` is the arena's high-water mark. It includes the allocator's 16-byte block headers, so it reads higher than `alloc live`.
- `budget` is the heap figure the collector paces against: three quarters of the arena (98,304 B), leaving the rest for the allocator's block headers and the VM's memory the collector does not track.

`boot heap:` prints once the VM is open with its standard library, and `ready:` once the verbs, the events, the workload and the tick are installed.
A `render:` line prints at the end of every render, including one cut short by a press or a pan, and gives the render's wall time.
Runtime diagnostics print as `[E] msg`, `[W] msg`, `[I] msg` or `[D] msg`.

## Definition of done for this port

Watch picocom on `/dev/ttyACM0` from a reset.

1. **Boot heap.** The `boot heap:` line shows `alloc live` under 49,152 B.
2. **First render.** The LCD fills in coarse tiles that refine down to single pixels, and the first `render:` line prints its time and live bytes.
3. **Re-render.** Each USER press prints a new `render:` line whose `alloc live` is within 1,024 B of the first one's.
4. **Ten presses.** After ten presses there is no `OutOfMemoryError` line and the board has not reset.

## Memory layout

The VM's heap is a 128 KB arena in `.bss` of internal SRAM, served by the component's bump-and-freelist allocator.
The stack runs down from the top of the 192 KB SRAM, 0x20030000, toward the end of `.bss`; on this build that leaves about 63 KB.
The link fails if the statics and the arena leave less than 16 KB for the stack.
The LCD framebuffer lives in the 8 MB SDRAM at 0xD0000000.

To put the VM's heap in SDRAM instead, add `-DURBI_HEAP_EXTERNAL_ADDR=0xD0080000UL -DURBI_HEAP_BYTES=1048576UL` to `DEFS` in this example's Makefile.
The SDRAM controller is initialised before the first allocation, and SDRAM is slower than internal SRAM.

## Measured

| What | Figure |
| --- | --- |
| Re-render drift, host 64-bit build at a 32 x 24 canvas | worst 144 B over ten renders |
| Firmware `mandelbrot.elf`, text / data / bss | 119,608 / 164 / 148,800 B |
| Firmware `mandelbrot.bin` | 119,776 B |
| End of `.bss` | 0x200203e4, leaving 64,540 B for the stack |
| Undefined symbols in the ELF | 0 |
| Boot heap (board) | from the board log |
| First render time (board) | from the board log |
| `alloc live` after the first render and after ten presses (board) | from the board log |

The `.bss` figure includes the 128 KB arena and the linker's 16.5 KB stack-and-heap reservation.

## Known limits

- A fault stops the board: the red LED pulses three times, pauses, and repeats until a reset.
- The gyro reports a small nonzero rate at rest. Turns below the workload's deadzone of 3,000 mdeg/s are ignored so the view does not drift; a board with a larger resting bias may still creep, and `GYRO_DEADZONE` in `mandelbrot.u` tunes it.
- The zoom only goes one way: each press halves the span, and a reset returns to the full view.
- The firmware has no compiler and no value formatter, so the workload prints nothing itself; every console line comes from the shim.
- TIM2 (the render tick) and the button's EXTI0 interrupt share one preemption priority: both call `urbi_inject_event`, whose ring has a single producer, and giving them different preemption priorities could let one preempt the other mid-inject and drop a press.

## File layout

```text
examples/stm32f4/mandelbrot/
├── Makefile                  # the build: HAL, BSP, the component, the bake step, the link
├── STM32F429ZITx_FLASH.ld    # linker script: 2 MB flash, 192 KB SRAM, stack at the top
├── main.c                    # boot, the render verbs, the heap lines, the main loop
├── mandelbrot.u              # the workload
├── stm32f4xx_hal_conf.h      # HAL module selection
├── stm32f4xx_it.c            # SysTick, EXTI0 (the button), TIM2 (the tick), fault handlers
├── README.md                 # this file
└── stubs/
    ├── libc_stubs.c          # memset, memcpy, memmove, memcmp, strlen, strcmp
    ├── stdio.h               # a small snprintf for the shim's lines
    └── string.h              # declarations for libc_stubs.c

components/stm32f4-hal-baremetal/   # the board component: allocator, clock, writer, LCD, gyro, button
```
