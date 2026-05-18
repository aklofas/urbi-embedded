/* SPDX-License-Identifier: BSD-3-Clause */
#include "mock_bsp.h"
#include "../../components/stm32f4-hal-baremetal/include/port_stm32f4.h"
#include "../../include/urbi/types.h"
#include "../../include/urbi/urbi.h"
#include <stdio.h>
#include <assert.h>

/* port_lcd_fill_rect_native applies two transforms before reaching the BSP:
 *   1. RGB565 (urbi) -> ARGB8888 (BSP) colour conversion with alpha=0xFF
 *   2. 90 degree CCW rotation from landscape 320x240 (logical) to portrait
 *      240x320 (physical): (x,y,w,h) -> (y, 320-x-w, h, w)
 * Tests below assert both transforms reach the mock BSP correctly. */

static void test_fill_rect_passes_args_to_bsp(void) {
    mock_bsp_reset();
    UValue args[5];
    args[0] = urbi_make_int(10);
    args[1] = urbi_make_int(20);
    args[2] = urbi_make_int(30);
    args[3] = urbi_make_int(40);
    args[4] = urbi_make_int(0xF800);   /* RGB565 pure red */
    UValue out;
    UValue nil_self = urbi_make_nil();
    int rc = port_lcd_fill_rect_native(NULL, nil_self, args, 5, &out);
    assert(rc == 0);
    assert(mock_lcd.call_count == 1);
    /* 90 CCW: px = y_logical = 20 */
    assert(mock_lcd.last_x == 20);
    /* py = LCD_PHYS_H - x_logical - w_logical = 320 - 10 - 30 = 280 */
    assert(mock_lcd.last_y == 280);
    /* pw = h_logical = 40, ph = w_logical = 30 */
    assert(mock_lcd.last_w == 40);
    assert(mock_lcd.last_h == 30);
    /* RGB565 0xF800 (R5=31, G6=0, B5=0) -> ARGB8888 alpha=0xFF, R=0xFF */
    assert(mock_lcd.last_color == 0xFFFF0000U);
    printf("test_fill_rect_passes_args_to_bsp PASS\n");
}

static void test_fill_rect_clamps_oob(void) {
    mock_bsp_reset();
    UValue args[5];
    args[0] = urbi_make_int(-5);
    args[1] = urbi_make_int(-10);
    args[2] = urbi_make_int(500);    /* > LCD_W = 320 */
    args[3] = urbi_make_int(500);    /* > LCD_H = 240 */
    args[4] = urbi_make_int(0x07E0); /* RGB565 pure green (G6=63) */
    UValue out;
    UValue nil_self = urbi_make_nil();
    int rc = port_lcd_fill_rect_native(NULL, nil_self, args, 5, &out);
    assert(rc == 0);
    /* Logical clamp: x=0, y=0, w=320, h=240.  After rotation:
     *   px = y_logical (0) = 0
     *   py = LCD_PHYS_H - x - w = 320 - 0 - 320 = 0
     *   pw = h_logical = 240
     *   ph = w_logical = 320 */
    assert(mock_lcd.last_x == 0);
    assert(mock_lcd.last_y == 0);
    assert(mock_lcd.last_w == 240);
    assert(mock_lcd.last_h == 320);
    /* RGB565 0x07E0 (G6=63) -> ARGB8888 alpha=0xFF, G=0xFF
     * (channel expansion: g8 = (g6 << 2) | (g6 >> 4) = 252 | 3 = 255) */
    assert(mock_lcd.last_color == 0xFF00FF00U);
    printf("test_fill_rect_clamps_oob PASS\n");
}

int main(void) {
    test_fill_rect_passes_args_to_bsp();
    test_fill_rect_clamps_oob();
    return 0;
}
