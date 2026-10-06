/* SPDX-License-Identifier: BSD-3-Clause */
/* examples/stm32f4/mandelbrot/main.c — the Mandelbrot demo's shim.
 *
 * Boot: HAL and the 180 MHz clock tree, SDRAM (the LCD framebuffer lives
 * there), LCD, gyro, USART1 (the ST-Link's virtual COM port), the user
 * button's EXTI line, then the VM over the component's 128 KB internal
 * arena, the board verbs and events, the baked workload, and TIM2's 50 ms
 * gyro tick last.  Loop: one urbi_step slice, sleep when nothing is
 * runnable.  Every line printed here goes to USART1. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>           /* the example's stub snprintf */

#include "stm32f4xx_hal.h"
#include "stm32f429i_discovery.h"
#include "stm32f429i_discovery_sdram.h"
#include "stm32f429i_discovery_lcd.h"
#include "port_stm32f4.h"
#include "urbi/urbi.h"
#include "mandelbrot_baked.h"   /* mandelbrot_wire[], mandelbrot_wire_len */

UART_HandleTypeDef huart1;      /* port_writer.c uses it */
TIM_HandleTypeDef  htim2;       /* stm32f4xx_it.c uses it */

#define STEP_BUDGET 256u

static UVM            *s_vm;
static urbi_event_id_t s_tick = URBI_EVENT_ID_INVALID;
static uint64_t        s_render_started_us;

/* Called from TIM2_IRQHandler every 50 ms. */
void port_gyro_tick_isr(void)
{
    if (s_vm != NULL && s_tick != URBI_EVENT_ID_INVALID) (void)urbi_inject_event(s_vm, s_tick, NULL, 0);
}

/* The fault handlers in stm32f4xx_it.c land here: the red LED pulses
 * three times, pauses, repeats, with busy-waits because no interrupt is
 * delivered inside a fault. */
void port_fault_loop(void)
{
    BSP_LED_Init(LED4);
    for (;;) {
        for (int i = 0; i < 3; i++) {
            BSP_LED_On(LED4);  for (volatile uint32_t k = 0; k < 15000000u; k++) { }
            BSP_LED_Off(LED4); for (volatile uint32_t k = 0; k <  5000000u; k++) { }
        }
        for (volatile uint32_t k = 0; k < 20000000u; k++) { }
    }
}

static void console(const char *s) { port_uart_write(s, strlen(s)); }

static void report_heap(const char *label)
{
    urbi_gc_collect(s_vm);
    UGcStats st; memset(&st, 0, sizeof st);
    (void)urbi_gc_stats(s_vm, &st);
    char line[160];
    int n = snprintf(line, sizeof line, "%s: alloc live %lu B, gc live %lu B, heap top %lu B, budget %lu B\r\n",
                     label, (unsigned long)port_alloc_live_bytes(), (unsigned long)st.bytes_live,
                     (unsigned long)port_alloc_heap_top(), (unsigned long)st.heap_budget);
    if (n > 0) port_uart_write(line, (size_t)n);
}

static void report_last_error(void)
{
    UErrorInfo info; memset(&info, 0, sizeof info);
    (void)urbi_last_error(s_vm, &info);
    console("  "); console(info.message ? info.message : "(no message)"); console("\r\n");
}

static int c_render_begin(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    s_render_started_us = port_time_us(NULL);
    *out = urbi_make_nil();
    return UEXEC_OK;
}

static int c_render_end(UVM *vm, UValue self, UValue *args, uint8_t nargs, UValue *out)
{
    (void)vm; (void)self; (void)args; (void)nargs;
    uint64_t ms = (port_time_us(NULL) - s_render_started_us) / 1000u;
    char line[160];
    int n = snprintf(line, sizeof line, "render: %lu ms, ", (unsigned long)ms);
    if (n > 0) port_uart_write(line, (size_t)n);
    report_heap("after render");
    *out = urbi_make_nil();
    return UEXEC_OK;
}

/* 180 MHz: HSE 8 MHz, PLL M=8 N=360 P=2 Q=7, over-drive, APB1 /4, APB2 /2. */
static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM = 8;
    osc.PLL.PLLN = 360;
    osc.PLL.PLLP = RCC_PLLP_DIV2;
    osc.PLL.PLLQ = 7;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) while (1);

    if (HAL_PWREx_EnableOverDrive() != HAL_OK) while (1);

    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV4;
    clk.APB2CLKDivider = RCC_HCLK_DIV2;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK) while (1);
}

/* TIM2 at 20 Hz: 90 MHz timer clock, prescaler 8,999 -> 10 kHz, period 499. */
static void tim2_init_50ms(void)
{
    __HAL_RCC_TIM2_CLK_ENABLE();

    htim2.Instance               = TIM2;
    htim2.Init.Prescaler         = 8999U;
    htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim2.Init.Period            = 499U;
    htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim2) != HAL_OK) while (1);

    /* Same preemption priority as EXTI0 (button): both handlers call
     * urbi_inject_event, whose ring has a single producer, and two
     * interrupts at different preemption priorities could preempt each
     * other mid-inject and drop an event. The sub-priority (1) only
     * orders two pending IRQs; it cannot let one preempt the other. */
    HAL_NVIC_SetPriority(TIM2_IRQn, 5, 1);
    HAL_NVIC_EnableIRQ(TIM2_IRQn);

    if (HAL_TIM_Base_Start_IT(&htim2) != HAL_OK) while (1);
}

/* The DWT cycle counter port_time_us reads. */
static void dwt_enable(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    dwt_enable();
    BSP_SDRAM_Init();
    port_lcd_init();
    port_gyro_init();
    port_uart_init();
    port_button_init_gpio();

    BSP_LCD_Clear(LCD_COLOR_BLACK);
    BSP_LCD_SetTextColor(LCD_COLOR_WHITE);
    BSP_LCD_SetBackColor(LCD_COLOR_BLACK);
    BSP_LCD_SetFont(&Font12);
    BSP_LCD_DisplayStringAtLine(0, (uint8_t *)"urbi " URBI_RELEASE_STRING);

    console("\r\n=== urbi "); console(urbi_version()); console(" on STM32F429I-DISC1 ===\r\n");

    UVMConfig cfg; memset(&cfg, 0, sizeof cfg);
    cfg.step_budget = STEP_BUDGET;
    cfg.boot_stdlib = 1;
    /* The collector paces what it tracks (objects, their arrays, loaded
     * code) against three quarters of this figure.  The allocator's block
     * headers and the VM's untracked memory come out of the same arena
     * on top of that, so the declared budget is three quarters of the
     * arena: declaring the whole arena let the high-water mark reach its
     * last few hundred bytes on the board. */
    cfg.heap_budget = port_alloc_heap_size() / 4u * 3u;

    console("[1] urbi_open... ");
    s_vm = urbi_open(port_alloc, NULL, &cfg);
    if (s_vm == NULL || urbi_realm_main(s_vm) == NULL) { console("FAILED\r\n"); port_fault_loop(); }
    urbi_set_clock(s_vm, port_time_us, NULL);
    urbi_set_writer(s_vm, port_writer, NULL);
    urbi_set_diag(s_vm, port_diag, NULL);
    console("ok\r\n");
    report_heap("boot heap");          /* the board's 32-bit boot number */

    console("[2] verbs and events... ");
    if (urbi_register(s_vm, "lcd_fill_rect", port_lcd_fill_rect_native, 5, 5) != URBI_OK ||
        urbi_register(s_vm, "gyro_x", port_gyro_x_native, 0, 0) != URBI_OK ||
        urbi_register(s_vm, "gyro_y", port_gyro_y_native, 0, 0) != URBI_OK ||
        urbi_register(s_vm, "gyro_z", port_gyro_z_native, 0, 0) != URBI_OK ||
        urbi_register(s_vm, "render_begin", c_render_begin, 0, 0) != URBI_OK ||
        urbi_register(s_vm, "render_end", c_render_end, 0, 0) != URBI_OK) {
        console("FAILED\r\n"); report_last_error(); port_fault_loop();
    }
    {
        urbi_event_id_t button = URBI_EVENT_ID_INVALID;
        UValue ev = urbi_make_nil();
        if (urbi_event_register(s_vm, NULL, "button_press", &button) != URBI_OK ||
            urbi_event_value(s_vm, button, &ev) != URBI_OK ||
            urbi_global_set(s_vm, NULL, "button_press", ev) != URBI_OK ||
            urbi_event_register(s_vm, NULL, "gyro_tick", &s_tick) != URBI_OK ||
            urbi_event_value(s_vm, s_tick, &ev) != URBI_OK ||
            urbi_global_set(s_vm, NULL, "gyro_tick", ev) != URBI_OK) {
            console("FAILED\r\n"); report_last_error(); port_fault_loop();
        }
        port_button_bind(s_vm, button);
    }
    console("ok\r\n");

    console("[3] workload... ");
    {
        UValue out = urbi_make_nil();
        if (urbi_load(s_vm, urbi_realm_main(s_vm), mandelbrot_wire, mandelbrot_wire_len, &out) != URBI_OK) {
            console("FAILED\r\n"); report_last_error(); port_fault_loop();
        }
    }
    console("ok\r\n");

    console("[4] tick... ");
    tim2_init_50ms();
    console("ok\r\n");
    report_heap("ready");

    for (;;) {
        uint64_t wake_us = 0;
        int st = urbi_step(s_vm, STEP_BUDGET, &wake_us);
        if (st < 0) { console("urbi_step failed; resetting\r\n"); report_last_error(); NVIC_SystemReset(); }
        if (st == URBI_STEP_QUIESCENT ||
            (st == URBI_STEP_IDLE_UNTIL && wake_us > port_time_us(NULL) + 50000u)) {
            __WFI();            /* TIM2 fires within 50 ms; EXTI0 wakes it too */
        }
    }
}
