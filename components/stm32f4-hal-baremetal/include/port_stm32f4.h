/* SPDX-License-Identifier: BSD-3-Clause */
/* STM32F4 bare-metal port glue — function prototypes for urbi <-> STM32CubeF4 BSP integration.
 *
 * Embedders include this header from their main.c and hand the functions
 * below to the urbi host hooks: port_alloc to urbi_open, port_time_us to
 * urbi_set_clock, port_writer to urbi_set_writer, port_diag to
 * urbi_set_diag, and the natives to urbi_register.  Each one's signature
 * matches the hook it satisfies (see include/urbi/urbi.h and
 * include/urbi/types.h for the canonical typedefs).
 *
 * Target board: STM32F429I-DISC1 (Cortex-M4F, 180 MHz, 256 KB SRAM, 8 MB SDRAM,
 * 2.4" 240x320 RGB565 LCD via LTDC/ILI9341, L3GD20 gyro via SPI5,
 * USER button on PA0 via EXTI0, ST-Link/V2-B for USART1 VCP + SWD).
 */
#ifndef URBI_PORT_STM32F4_H
#define URBI_PORT_STM32F4_H

#include <stddef.h>
#include <stdint.h>
#include "urbi/types.h"   /* UValue, struct UVM (opaque), UVMAllocFn, urbi_event_id_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Compile-time tunables: override via -D... at build time. */
#ifndef URBI_HEAP_BYTES
#  define URBI_HEAP_BYTES  (128U * 1024U)
#endif

/* Allocator over a static heap of URBI_HEAP_BYTES (default 128 KB).
 * Realloc-shaped, matching UVMAllocFn: pass it to urbi_open. */
void *port_alloc(void *ptr, size_t nbytes, void *ud);

/* Requested bytes outstanding: what callers asked for and still hold,
 * header overhead excluded.  The figure the 48 KB target caps. */
size_t port_alloc_live_bytes(void);

/* Bump high-water mark in bytes, header overhead included. */
size_t port_alloc_heap_top(void);

/* Arena size in bytes (URBI_HEAP_BYTES). */
size_t port_alloc_heap_size(void);

/* Number of successful allocations since boot. */
size_t port_alloc_count(void);

/* Number of frees since boot. */
size_t port_alloc_free_count(void);

/* Number of allocations served from the freelist instead of the bump. */
size_t port_alloc_freelist_hits(void);

/* Block size (header included) of the last request the arena refused. */
size_t port_alloc_last_failed_request(void);

/* Monotonic microseconds from the DWT cycle counter at 180 MHz, widened
 * to 64 bits.  The clock hook: pass to urbi_set_clock. */
uint64_t port_time_us(void *ud);

/* Script output over USART1 as "[chan] msg".  The writer hook: pass to
 * urbi_set_writer. */
void port_writer(void *ud, const char *chan, size_t chan_len,
                 const char *msg, size_t msg_len);

/* Runtime diagnostics over USART1 with a [D]/[I]/[W]/[E] prefix chosen
 * from the syslog level.  The diag hook: pass to urbi_set_diag. */
void port_diag(struct UVM *vm, void *ud, int level, const char *msg, size_t len);

/* USART1 init: 115200 8N1 on PA9 (TX) / PA10 (RX), routed to the
 * ST-Link VCP.  Call once from main() before the VM is opened. */
void port_uart_init(void);

/* Raw, blocking write of `n` bytes to USART1; the shim's own console. */
void port_uart_write(const char *s, size_t n);

/* LCD init: LTDC + ILI9341 with one layer in SDRAM at 0xD0000000.  Call
 * once from main() after BSP_SDRAM_Init(). */
void port_lcd_init(void);

/* Native lcd_fill_rect(x, y, w, h, rgb565) on a 320x240 landscape
 * canvas, clamped to the screen.  Register with exactly five arguments. */
int port_lcd_fill_rect_native(struct UVM *vm, UValue self,
                              UValue *args, uint8_t nargs, UValue *out);

/* Gyro init: L3GD20 over SPI5 via BSP_GYRO_Init.  Call once from main()
 * after HAL_Init(). */
void port_gyro_init(void);

/* Native gyro_x(): one L3GD20 read, the X axis as a float.  Register
 * with no arguments. */
int port_gyro_x_native(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                       UValue *out);

/* Native gyro_y(): one L3GD20 read, the Y axis as a float.  Register
 * with no arguments. */
int port_gyro_y_native(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                       UValue *out);

/* Native gyro_z(): one L3GD20 read, the Z axis as a float.  Register
 * with no arguments. */
int port_gyro_z_native(struct UVM *vm, UValue self, UValue *args, uint8_t nargs,
                       UValue *out);

/* USER button GPIO: PA0 as input with a rising-edge EXTI0 interrupt. */
void port_button_init_gpio(void);

/* Bind the button to a VM and an event id from urbi_event_register; the
 * EXTI0 handler injects this id on every press. */
void port_button_bind(struct UVM *vm, urbi_event_id_t id);

/* Call from EXTI0_IRQHandler: injects the bound event, if any. */
void port_button_exti_handler(void);

#ifdef __cplusplus
}
#endif

#endif /* URBI_PORT_STM32F4_H */
