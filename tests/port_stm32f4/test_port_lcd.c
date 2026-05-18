/* SPDX-License-Identifier: BSD-3-Clause */
#include "mock_bsp.h"
#include "../../components/stm32f4-hal-baremetal/include/port_stm32f4.h"
#include "../../include/urbi/types.h"
#include "../../include/urbi/urbi.h"
#include <stdio.h>
#include <assert.h>

static void test_fill_rect_passes_args_to_bsp(void) {
    mock_bsp_reset();
    UValue args[5];
    args[0] = urbi_make_int(10);
    args[1] = urbi_make_int(20);
    args[2] = urbi_make_int(30);
    args[3] = urbi_make_int(40);
    args[4] = urbi_make_int(0xF800);   /* RGB565 red */
    UValue out;
    int rc = port_lcd_fill_rect_native(NULL, args, 5, &out);
    assert(rc == 0);  /* URBI_EXEC_OK */
    assert(mock_lcd.call_count == 1);
    assert(mock_lcd.last_x == 10);
    assert(mock_lcd.last_y == 20);
    assert(mock_lcd.last_w == 30);
    assert(mock_lcd.last_h == 40);
    assert(mock_lcd.last_color == 0xF800);
    printf("test_fill_rect_passes_args_to_bsp PASS\n");
}

static void test_fill_rect_clamps_oob(void) {
    mock_bsp_reset();
    UValue args[5];
    args[0] = urbi_make_int(-5);
    args[1] = urbi_make_int(-10);
    args[2] = urbi_make_int(500);    /* > 320 */
    args[3] = urbi_make_int(500);    /* > 240 */
    args[4] = urbi_make_int(0x07E0);
    UValue out;
    int rc = port_lcd_fill_rect_native(NULL, args, 5, &out);
    assert(rc == 0);
    /* Expect clamping: x,y to >=0; x+w <= 320; y+h <= 240 */
    assert(mock_lcd.last_x == 0);
    assert(mock_lcd.last_y == 0);
    assert(mock_lcd.last_w <= 320);
    assert(mock_lcd.last_h <= 240);
    printf("test_fill_rect_clamps_oob PASS\n");
}

int main(void) {
    test_fill_rect_passes_args_to_bsp();
    test_fill_rect_clamps_oob();
    return 0;
}
