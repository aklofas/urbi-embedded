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

#define LCD_W  320
#define LCD_H  240

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
    /* v0.8.2 bring-up: count calls + print every 256th via port_writer
     * (avoid pulling vm/uvm.h into the port shim).  Remove before tag. */
#ifndef URBI_PORT_TEST
    static uint32_t s_call_count = 0;
    s_call_count++;
    if ((s_call_count & 0xFFU) == 1U) {
        char b[24];
        const char *d = "0123456789ABCDEF";
        int n = 0;
        const char *t = "fill n=";
        while (t[n] && n < 7) { b[n] = t[n]; n++; }
        for (int k = 28; k >= 0; k -= 4) b[n++] = d[(s_call_count >> k) & 0xF];
        b[n++] = '\r'; b[n++] = '\n';
        port_writer(NULL, "lcd", 3, b, (size_t)n, 0);
    }
#endif
    if (nargs != 5) {
        *out = urbi_make_nil();
        return -1;  /* URBI_EXEC_ERR_ARITY */
    }
    int32_t x = (int32_t)urbi_value_as_int(args[0]);
    int32_t y = (int32_t)urbi_value_as_int(args[1]);
    int32_t w = (int32_t)urbi_value_as_int(args[2]);
    int32_t h = (int32_t)urbi_value_as_int(args[3]);
    uint32_t color = (uint32_t)urbi_value_as_int(args[4]);
#ifndef URBI_PORT_TEST
    /* v0.8.2 bring-up: dump args for the first 4 calls.  All-black LCD with
     * fills happening = colors all zero or args misaligned.  Remove with
     * the count tap before tag. */
    if (s_call_count <= 4U) {
        char b[80];
        const char *d = "0123456789ABCDEF";
        int n = 0;
        const char *t = "arg x="; while (t[n] && n < 6) { b[n] = t[n]; n++; }
        for (int k = 28; k >= 0; k -= 4) b[n++] = d[((uint32_t)x >> k) & 0xF];
        b[n++] = ' '; b[n++] = 'y'; b[n++] = '=';
        for (int k = 28; k >= 0; k -= 4) b[n++] = d[((uint32_t)y >> k) & 0xF];
        b[n++] = ' '; b[n++] = 'w'; b[n++] = '=';
        for (int k = 28; k >= 0; k -= 4) b[n++] = d[((uint32_t)w >> k) & 0xF];
        b[n++] = ' '; b[n++] = 'h'; b[n++] = '=';
        for (int k = 28; k >= 0; k -= 4) b[n++] = d[((uint32_t)h >> k) & 0xF];
        b[n++] = ' '; b[n++] = 'c'; b[n++] = '=';
        for (int k = 28; k >= 0; k -= 4) b[n++] = d[(color >> k) & 0xF];
        b[n++] = '\r'; b[n++] = '\n';
        port_writer(NULL, "lcd", 3, b, (size_t)n, 0);
    }
#endif

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
    BSP_LCD_FillRect((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h);
    *out = urbi_make_nil();
    return 0;
}
