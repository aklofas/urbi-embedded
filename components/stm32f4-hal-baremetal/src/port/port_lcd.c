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
    (void)self;
    /* v0.8.2 bring-up: count calls + print every 256th.  Confirms whether
     * the urbiscript is reaching lcd_fill_rect at all and how fast.
     * Remove before tag. */
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
        if (vm && vm->writer_fn) vm->writer_fn(vm->writer_ud, "lcd", 3,
                                                b, (size_t)n, 0);
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

    BSP_LCD_SetTextColor(color);
    BSP_LCD_FillRect((uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h);
    *out = urbi_make_nil();
    return 0;
}
