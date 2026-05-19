/* SPDX-License-Identifier: BSD-3-Clause */
/* Mandelbrot demo for STM32F429I-DISC1.
 *
 * Bare-metal init: HAL clocks at 180 MHz, SDRAM, LCD, gyro, UART, button GPIO,
 * TIM2 (50 ms periodic tick for gyro_tick event injection).
 * Then drops into the urbi event pump: load baked bytecode, urbi_step loop
 * with WFI sleep when QUIESCENT, SystemReset on FATAL. */

#include "stm32f4xx_hal.h"
#include "stm32f429i_discovery.h"
#include "stm32f429i_discovery_sdram.h"
#include "stm32f429i_discovery_lcd.h"
#include "stm32f429i_discovery_gyroscope.h"
#include "port_stm32f4.h"
#include "urbi/urbi.h"
#include "urbi/types.h"
#include <string.h>  /* strlen from libc_stubs.c — needed to print errbuf cleanly */
#include <stdio.h>   /* snprintf from stubs/stdio.h — diagnostic formatting */
/* Full UVM struct definition needed for static allocation.
 * The public API only forward-declares the type; embedders that
 * allocate UVM in BSS must pull the internal header — same pattern
 * as tests/qemu/reactive_smoke/main/reactive_smoke_main.c. */
#include "vm/uvm.h"
#include "chunk/uchunk.h"  /* freestanding-safe uchunk_deserialize / UChunkLoadError */
#include "mandelbrot_baked.h"   /* mandelbrot_bytecode[] + mandelbrot_bytecode_size */

UART_HandleTypeDef huart1;   /* defined here; port_writer.c uses extern */

/* TIM2 handle — defined here; TIM2_IRQHandler in stm32f4xx_it.c uses extern. */
TIM_HandleTypeDef htim2;

/* VM pointer + gyro_tick event ID — set before TIM2 IRQ is enabled. */
static struct UVM       *s_vm;
static urbi_event_id_t   s_gyro_tick_evt = URBI_EVENT_ID_INVALID;

/* Called from TIM2_IRQHandler (stm32f4xx_it.c) every 50 ms. */
void port_gyro_tick_isr(void) {
    if (s_vm != NULL && s_gyro_tick_evt != URBI_EVENT_ID_INVALID) {
        urbi_inject_event(s_vm, (uint32_t)s_gyro_tick_evt, NULL, 0U);
    }
}


/* DWT enable — needed by port_time.c for DWT->CYCCNT access. */
static void dwt_enable(void) {
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

/* 180 MHz clock config — copy/adapt from STM32CubeF4 Templates main.c. */
static void SystemClock_Config(void) {
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

/* TIM2 init — 50 ms period (20 Hz) for gyro_tick injection.
 *
 * APB1 clock = SYSCLK / APB1Div = 180 MHz / 4 = 45 MHz.
 * TIM2 input clock = 2 * APB1 = 90 MHz (APB1Div != 1, so timer clock doubled).
 *
 * Prescaler = 8999 → timer tick = 90 MHz / (8999+1) = 10 000 Hz = 100 µs.
 * Period    = 499  → overflow period = (499+1) * 100 µs = 50 ms (20 Hz).
 *
 * Both values fit in 16-bit (TIM2 is 32-bit on F4, but values are small). */
static void tim2_init_50ms(void) {
    __HAL_RCC_TIM2_CLK_ENABLE();

    htim2.Instance               = TIM2;
    htim2.Init.Prescaler         = 8999U;
    htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim2.Init.Period            = 499U;
    htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim2) != HAL_OK) while (1);

    HAL_NVIC_SetPriority(TIM2_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(TIM2_IRQn);

    if (HAL_TIM_Base_Start_IT(&htim2) != HAL_OK) while (1);
}

int main(void) {
    HAL_Init();
    SystemClock_Config();
    dwt_enable();

    BSP_SDRAM_Init();

    /* SDRAM probe: write a pattern at the urbi heap base, read it back.
     * Catches a non-functional SDRAM controller before urbi tries to alloc
     * 1 MB into it.  Done before port_uart_init so we may not see the
     * panic on UART — but the LCD splash WILL show, which is enough
     * signal: if SDRAM is dead the LCD framebuffer at 0xD0000000 wouldn't
     * be writable either. */
    {
        volatile uint32_t *sdram_probe = (volatile uint32_t *)0xD0080000UL;
        *sdram_probe       = 0xDEADBEEFU;
        *(sdram_probe + 1) = 0xCAFEBABEU;
        if (*sdram_probe != 0xDEADBEEFU || *(sdram_probe + 1) != 0xCAFEBABEU) {
            while (1);  /* SDRAM @ urbi heap base not writable */
        }
    }

    port_lcd_init();
    port_gyro_init();
    port_uart_init();
    port_button_init_gpio();

    /* Splash */
    BSP_LCD_Clear(LCD_COLOR_BLACK);
    BSP_LCD_SetTextColor(LCD_COLOR_WHITE);
    BSP_LCD_SetBackColor(LCD_COLOR_BLACK);
    BSP_LCD_SetFont(&Font12);
    BSP_LCD_DisplayStringAtLine(0, (uint8_t *)"urbi v0.8.2-stm32f4-mandelbrot");

    /* Hello UART */
    static const char hello[] = "urbi v0.8.2-stm32f4-mandelbrot booting\r\n";
    HAL_UART_Transmit(&huart1, (const uint8_t *)hello, sizeof hello - 1U, 100);

    /* Stand up the VM — UVM is BSS-allocated; urbi_vm_init fills it. */
    static struct UVM vm;
    if (urbi_vm_init(&vm, port_alloc, NULL) != 0) {
        static const char err[] = "urbi_vm_init FAILED\r\n";
        HAL_UART_Transmit(&huart1, (const uint8_t *)err, sizeof err - 1U, 100);
        while (1);
    }
    urbi_set_writer   (&vm, port_writer, NULL);
    urbi_set_time_us  (&vm, port_time_us);
    urbi_set_diag_fn  (&vm, port_diag);
    urbi_set_isr_check_fn(&vm, port_in_isr);

    /* Obtain the default realm — NULL arg means global realm. */
    struct URealm *realm = urbi_realm_global(&vm);
    if (realm == NULL) {
        static const char err[] = "urbi_realm_global returned NULL (OOM during realm create)\r\n";
        HAL_UART_Transmit(&huart1, (const uint8_t *)err, sizeof err - 1U, 100);
        while (1);
    }

    /* Register host-fns — bail fast on any failure. */
    #define REG_OR_DIE(name_, fn_)                                              \
        do {                                                                    \
            int _rc = urbi_register(&vm, realm, (name_), (fn_));                \
            if (_rc != 0) {                                                     \
                char _buf[128];                                                 \
                int  _n = snprintf(_buf, sizeof _buf,                           \
                                   "urbi_register(\"%s\") FAILED rc=%d\r\n",   \
                                   (name_), _rc);                              \
                if (_n > 0) HAL_UART_Transmit(&huart1, (const uint8_t *)_buf,  \
                                              (uint16_t)_n, 100);              \
                while (1);                                                      \
            }                                                                   \
        } while (0)
    REG_OR_DIE("lcd_fill_rect", port_lcd_fill_rect_native);
    REG_OR_DIE("gyro_x",        port_gyro_x_native);
    REG_OR_DIE("gyro_y",        port_gyro_y_native);
    REG_OR_DIE("gyro_z",        port_gyro_z_native);
    #undef REG_OR_DIE

    /* Register the button event and bind to EXTI ISR. */
    urbi_event_id_t button_evt = urbi_event_register(&vm, realm,
                                                      "button_press",
                                                      NULL, NULL);
    if (button_evt == URBI_EVENT_ID_INVALID) {
        urbi_error_info_t einfo = {0};
        (void)urbi_last_error(&vm, &einfo);
        char buf[160];
        int n = snprintf(buf, sizeof buf,
                         "urbi_event_register(\"button_press\") FAILED: err=%d msg=%s\r\n",
                         einfo.code, einfo.message ? einfo.message : "(none)");
        if (n > 0) HAL_UART_Transmit(&huart1, (const uint8_t *)buf,
                                     (uint16_t)n, 100);
        while (1);
    }
    port_button_init(&vm, (uint32_t)button_evt);

    /* Register the gyro_tick event (injected every 50 ms by TIM2 ISR).
     * Store vm pointer + event id in file-statics before enabling the IRQ. */
    s_gyro_tick_evt = urbi_event_register(&vm, realm, "gyro_tick", NULL, NULL);
    if (s_gyro_tick_evt == URBI_EVENT_ID_INVALID) {
        urbi_error_info_t einfo = {0};
        (void)urbi_last_error(&vm, &einfo);
        char buf[160];
        int n = snprintf(buf, sizeof buf,
                         "urbi_event_register(\"gyro_tick\") FAILED: err=%d msg=%s\r\n",
                         einfo.code, einfo.message ? einfo.message : "(none)");
        if (n > 0) HAL_UART_Transmit(&huart1, (const uint8_t *)buf,
                                     (uint16_t)n, 100);
        while (1);
    }
    s_vm = &vm;

    /* Start TIM2 — gyro_tick ISR fires from here onward.  port_alloc
     * recycles freed strand stacks via its freelist (v0.8.2), so the
     * 50 ms tick rate no longer drains the heap. */
    tim2_init_50ms();

    /* Load baked bytecode (freestanding pattern: static UModule + uchunk_deserialize).
     * urbi_module_from_bytes is __STDC_HOSTED__-gated and returns NULL on bare-metal.
     *
     * IMPORTANT: caller MUST set module->alloc_fn / alloc_ud before deserialize.
     * module_allocator() in freestanding mode returns c->alloc_fn directly (no
     * malloc fallback); NULL there → immediate UCHUNK_LOAD_OOM at umodule.c:1118. */
    static UModule mod = {0};
    mod.alloc_fn = port_alloc;
    mod.alloc_ud = NULL;
    char errbuf[128] = {0};
    UChunkLoadError lerr = uchunk_deserialize(&mod, mandelbrot_bytecode,
                                                 mandelbrot_bytecode_size,
                                                 errbuf, sizeof errbuf);
    if (lerr != UCHUNK_LOAD_OK) {
        static const char prefix[] = "uchunk_deserialize FAILED: ";
        HAL_UART_Transmit(&huart1, (const uint8_t *)prefix, sizeof prefix - 1U, 100);
        const char *name = uchunk_load_error_name(lerr);
        HAL_UART_Transmit(&huart1, (const uint8_t *)name, strlen(name), 100);
        if (errbuf[0] != '\0') {
            HAL_UART_Transmit(&huart1, (const uint8_t *)" - ", 3U, 100);
            HAL_UART_Transmit(&huart1, (const uint8_t *)errbuf, strlen(errbuf), 100);
        }
        HAL_UART_Transmit(&huart1, (const uint8_t *)"\r\n", 2U, 100);

        /* Heap diagnostics — only meaningful on UCHUNK_LOAD_OOM but harmless otherwise. */
        extern size_t port_alloc_heap_top(void);
        extern size_t port_alloc_heap_size(void);
        extern const void *port_alloc_heap_base(void);
        extern size_t port_alloc_last_failed_request(void);
        extern size_t port_alloc_count(void);
        extern size_t port_alloc_largest_satisfied(void);
        extern const void *port_alloc_last_null_ptr(void);
        extern size_t port_alloc_last_null_nbytes(void);
        char dbgbuf[224];
        int n = snprintf(dbgbuf, sizeof dbgbuf,
                         "heap: base=%p size=%lu used=%lu\r\n"
                         "      count=%lu largest_ok=%lu failed_req=%lu\r\n"
                         "      last_null: ptr=%p nbytes=%lu\r\n",
                         port_alloc_heap_base(),
                         (unsigned long)port_alloc_heap_size(),
                         (unsigned long)port_alloc_heap_top(),
                         (unsigned long)port_alloc_count(),
                         (unsigned long)port_alloc_largest_satisfied(),
                         (unsigned long)port_alloc_last_failed_request(),
                         port_alloc_last_null_ptr(),
                         (unsigned long)port_alloc_last_null_nbytes());
        if (n > 0) {
            HAL_UART_Transmit(&huart1, (const uint8_t *)dbgbuf,
                              (uint16_t)(n < (int)sizeof dbgbuf ? n : (int)sizeof dbgbuf - 1),
                              100);
        }
        while (1);
    }

    {
        int rcc = urbi_run_chunk(&vm, realm, &mod, NULL);
        if (rcc != 0) {
            urbi_error_info_t einfo = {0};
            (void)urbi_last_error(&vm, &einfo);
            char rcbuf[224];
            int n = snprintf(rcbuf, sizeof rcbuf,
                             "urbi_run_chunk FAILED: rc=%d  err=%d\r\n"
                             "  context=%s\r\n  message=%s\r\n  src=%s:%d\r\n",
                             rcc, einfo.code,
                             einfo.context     ? einfo.context     : "(none)",
                             einfo.message     ? einfo.message     : "(none)",
                             einfo.source_name ? einfo.source_name : "(none)",
                             einfo.source_line);
            if (n > 0) {
                HAL_UART_Transmit(&huart1, (const uint8_t *)rcbuf,
                                  (uint16_t)(n < (int)sizeof rcbuf ? n : (int)sizeof rcbuf - 1),
                                  100);
            }
            while (1);
        }
    }

    /* Event pump. */
    while (1) {
        uint64_t wake_us = 0U;
        UStepResult st = urbi_step(&vm, 256U, &wake_us);
        switch (st) {
            case URBI_STEP_RUNNING:
                break;
            case URBI_STEP_QUIESCENT:
            case URBI_STEP_WAKE_AT:
                __WFI();
                break;
            case URBI_STEP_FATAL:
                NVIC_SystemReset();
                break;
            default:
                break;
        }
    }
}
