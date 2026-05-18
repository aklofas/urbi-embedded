/* SPDX-License-Identifier: BSD-3-Clause */
/* LCD wrapper for STM32F429I-DISC1 — exposes BSP_LCD_FillRect as the
 * lcd_fill_rect(x, y, w, h, color_rgb565) host-fn.
 *
 * Clamps coordinates to the visible 320x240 frame to avoid out-of-bounds
 * writes from urbi script (which may otherwise corrupt SDRAM regions
 * outside the framebuffer).
 *
 * port_lcd_init() drives BSP_LCD_Init + LayerDefaultInit for the demo's
 * needs (one RGB565 layer at 0xD0000000).  Must be called AFTER
 * BSP_SDRAM_Init() from main(). */

#include "port_stm32f4.h"
#include "urbi/types.h"
#include <stdint.h>

#ifdef URBI_PORT_TEST
#  include "mock_bsp.h"
#else
#  include "stm32f429i_discovery_lcd.h"
#  include "stm32f429i_discovery_sdram.h"
#endif

/* Physical LCD on STM32F429I-DISC1 is portrait 240×320 (BSP_LCD_GetXSize
 * = 240, GetYSize = 320).  The mandelbrot urbiscript assumes a landscape
 * 320×240 logical surface, so port_lcd_fill_rect_native rotates 90°
 * counter-clockwise when forwarding to BSP_LCD_FillRect:
 *   px = py_logical
 *   py = LCD_PHYS_H - 1 - x_logical - w_logical (mirror, so top-left
 *        of landscape lands at top-left of portrait when held landscape)
 *   pw = h_logical
 *   ph = w_logical
 * Logical bounds clamping uses the LANDSCAPE dimensions so urbi sees a
 * 320-wide × 240-tall canvas as advertised. */
#define LCD_PHYS_W 240
#define LCD_PHYS_H 320
#define LCD_W      320   /* logical (landscape) */
#define LCD_H      240   /* logical (landscape) */

void port_lcd_init(void) {
#ifndef URBI_PORT_TEST
    BSP_LCD_Init();
    BSP_LCD_LayerDefaultInit(0, 0xD0000000UL);
    BSP_LCD_SelectLayer(0);
    BSP_LCD_DisplayOn();
    BSP_LCD_Clear(LCD_COLOR_BLACK);
#endif
}

int port_lcd_fill_rect_native(struct UVM *vm, UValue self,
                              UValue *args, uint8_t nargs, UValue *out) {
    (void)vm; (void)self;
    if (nargs != 5) {
        *out = urbi_make_nil();
        return -1;  /* URBI_EXEC_ERR_ARITY */
    }
    int32_t x = (int32_t)urbi_value_as_int(args[0]);
    int32_t y = (int32_t)urbi_value_as_int(args[1]);
    int32_t w = (int32_t)urbi_value_as_int(args[2]);
    int32_t h = (int32_t)urbi_value_as_int(args[3]);
    uint32_t color = (uint32_t)urbi_value_as_int(args[4]);

    /* Clamp negative origin */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    /* Clamp width/height to fit screen */
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    /* Reject degenerate rectangles */
    if (w <= 0 || h <= 0) {
        *out = urbi_make_nil();
        return 0;
    }

    /* BSP_LCD_SetTextColor expects ARGB8888 (top byte = alpha).  The urbi
     * script computes RGB565 values; passing them straight in leaves
     * alpha=0 (fully transparent) and BSP_LCD_FillRect becomes a no-op.
     * Convert: RGB565 -> ARGB8888 with alpha=0xFF, expanding each channel
     * from 5/6/5 to 8/8/8 bits. */
    uint32_t r5 = (color >> 11) & 0x1FU;
    uint32_t g6 = (color >> 5)  & 0x3FU;
    uint32_t b5 =  color        & 0x1FU;
    uint32_t argb = 0xFF000000U
                  | ((r5 << 3 | r5 >> 2) << 16)
                  | ((g6 << 2 | g6 >> 4) << 8)
                  |  (b5 << 3 | b5 >> 2);
    BSP_LCD_SetTextColor(argb);

    /* 90° CCW rotation: landscape (x,y,w,h) -> portrait (px,py,pw,ph).
     * Maps logical top-left (0,0) to physical bottom-left (0, LCD_PHYS_H-1)
     * so when the board is held landscape-oriented (long edge horizontal),
     * the image reads naturally. */
    int32_t px = y;
    int32_t py = LCD_PHYS_H - x - w;
    int32_t pw = h;
    int32_t ph = w;
    if (px < 0) { pw += px; px = 0; }
    if (py < 0) { ph += py; py = 0; }
    if (pw <= 0 || ph <= 0) { *out = urbi_make_nil(); return 0; }
    BSP_LCD_FillRect((uint16_t)px, (uint16_t)py, (uint16_t)pw, (uint16_t)ph);
    *out = urbi_make_nil();
    return 0;
}
