/* SPDX-License-Identifier: BSD-3-Clause */
/* STM32F4 bare-metal port glue — function prototypes for urbi <-> STM32CubeF4 BSP integration.
 *
 * Embedders include this header from their main.c and pass the wrappers below
 * into the corresponding urbi register / init hooks (urbi_vm_init,
 * urbi_set_writer, urbi_set_time_us, urbi_set_diag_fn, ...).
 *
 * Each wrapper has a signature compatible with the urbi public hook typedef
 * it satisfies (UVMAllocFn, urbi_writer_fn, urbi_native_method_fn, ...).
 * See include/urbi/types.h and include/urbi/urbi.h for canonical typedefs.
 *
 * Target board: STM32F429I-DISC1 (Cortex-M4F, 180 MHz, 256 KB SRAM, 8 MB SDRAM,
 * 2.4" 240x320 RGB565 LCD via LTDC/ILI9341, L3GD20 gyro via SPI5,
 * USER button on PA0 via EXTI0, ST-Link/V2-B for USART1 VCP + SWD).
 */
#ifndef URBI_PORT_STM32F4_H
#define URBI_PORT_STM32F4_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "urbi/types.h"   /* UValue, struct UVM (opaque), UVMAllocFn */

#ifdef __cplusplus
extern "C" {
#endif

/* struct UVM is forward-declared in <urbi/types.h> */

/* Compile-time tunables: override via -D... at build time. */
#ifndef URBI_HEAP_BYTES
#  define URBI_HEAP_BYTES  (80U * 1024U)
#endif

/* Allocator: static .bss heap carved from internal SRAM.  Signature matches
 * UVMAllocFn (include/urbi/types.h) — pass as the alloc_fn parameter to
 * urbi_vm_init.  The heap is statically sized at URBI_HEAP_BYTES (default
 * 80 KB). */
void *port_alloc(void *ptr, size_t nbytes, void *ud);

/* Monotonic-microseconds time source.  Signature matches urbi_time_us_fn
 * (include/urbi/urbi.h) — pass to urbi_set_time_us.  Backed by the DWT
 * cycle counter at 180 MHz; resolution is 1 cycle (~5.5 ns), wraps at
 * uint64_t (~73 years). */
uint64_t port_time_us(void);

/* Channel-dispatching writer over USART1 (ST-Link VCP).  Signature matches
 * urbi_writer_fn (include/urbi/urbi.h) — pass to urbi_set_writer.  All
 * channels route to USART1; the channel name is printed as a prefix
 * (e.g. "[cerr] message\n"). */
void port_writer(void *ud,
                 const char *channel, size_t channel_len,
                 const char *msg,     size_t msg_len,
                 uint64_t ts_us);

/* ISR-context predicate.  Signature matches bool (*)(void) accepted by
 * urbi_set_isr_check_fn(vm, fn).  Backed by __get_IPSR() != 0. */
bool port_in_isr(void);

/* Runtime diagnostic channel routed to USART1 via port_writer.  Signature
 * matches urbi_diag_fn (include/urbi/urbi.h) — pass to urbi_set_diag_fn.
 * Levels: URBI_LOG_DEBUG/INFO/WARN/ERROR printed with [D]/[I]/[W]/[E]
 * prefix.  vsnprintf into a 192-byte stack buffer; truncates silently. */
void port_diag(struct UVM *vm, int level, const char *fmt, ...);

/* USART1 init: 115200 8N1 on PA9 (TX) / PA10 (RX), routed to ST-Link VCP.
 * Call once from main() before the urbi VM is created. */
void port_uart_init(void);

/* LCD init: brings up LTDC + ILI9341 + framebuffer in SDRAM at 0xD0000000.
 * Call once from main() AFTER BSP_SDRAM_Init().  Wraps the BSP sequence. */
void port_lcd_init(void);

/* LCD host-fn: registered into urbi as "lcd_fill_rect".  Signature matches
 * urbi_native_method_fn (include/urbi/urbi.h).  Fills an axis-aligned
 * rectangle at (x, y) of size (w, h) with the given RGB565 color via
 * BSP_LCD_FillRect.  Clamps to LCD bounds. */
int port_lcd_fill_rect_native(struct UVM *vm, UValue self,
                              UValue *args, uint8_t nargs, UValue *out);

/* Gyro init: brings up L3GD20 over SPI5 via BSP_GYRO_Init.  Call once
 * from main() AFTER HAL_Init(). */
void port_gyro_init(void);

/* Gyro host-fns: registered into urbi as "gyro_x", "gyro_y", "gyro_z".
 * Signature matches urbi_native_method_fn.  Each reads the L3GD20 once
 * via BSP_GYRO_GetXYZ and returns the requested axis in radians/sec
 * as a UVAL_FLOAT.  Reading 3 axes = 3 SPI transactions; acceptable for
 * the demo's 50ms tick (60 reads/sec is well under L3GD20's 800 Hz max). */
int port_gyro_x_native(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                       UValue *out);
int port_gyro_y_native(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                       UValue *out);
int port_gyro_z_native(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                       UValue *out);

/* Button GPIO init: configures PA0 as input with EXTI0 rising-edge IRQ.
 * Call BEFORE port_button_init(). */
void port_button_init_gpio(void);

/* Button event binding: associates the EXTI0 ISR with a urbi event ID
 * (returned by urbi_event_register).  After this call, every USER button
 * press emits the event into the urbi event ring from ISR context. */
void port_button_init(struct UVM *vm, uint32_t event_id);

#ifdef __cplusplus
}
#endif

#endif /* URBI_PORT_STM32F4_H */
