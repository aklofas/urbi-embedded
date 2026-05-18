/* SPDX-License-Identifier: BSD-3-Clause */
/* HAL feature gates for the Mandelbrot demo.  Enable only what we use. */
#ifndef STM32F4xx_HAL_CONF_H
#define STM32F4xx_HAL_CONF_H

#define HAL_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED
#define HAL_LTDC_MODULE_ENABLED
#define HAL_SDRAM_MODULE_ENABLED
#define HAL_SPI_MODULE_ENABLED
#define HAL_TIM_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED

#define HSE_VALUE              ((uint32_t)8000000)
#define HSI_VALUE              ((uint32_t)16000000)
#define LSI_VALUE              ((uint32_t)32000)
#define LSE_VALUE              ((uint32_t)32768)
#define EXTERNAL_CLOCK_VALUE   ((uint32_t)12288000)
#define VDD_VALUE              ((uint32_t)3300)
#define TICK_INT_PRIORITY      ((uint32_t)0x0F)
#define USE_RTOS               0
#define PREFETCH_ENABLE        1
#define INSTRUCTION_CACHE_ENABLE 1
#define DATA_CACHE_ENABLE      1

#include "stm32f4xx_hal_rcc.h"
#include "stm32f4xx_hal_gpio.h"
#include "stm32f4xx_hal_dma.h"
#include "stm32f4xx_hal_cortex.h"
#include "stm32f4xx_hal_pwr.h"
#include "stm32f4xx_hal_flash.h"
#include "stm32f4xx_hal_uart.h"
#include "stm32f4xx_hal_ltdc.h"
#include "stm32f4xx_hal_sdram.h"
#include "stm32f4xx_hal_spi.h"
#include "stm32f4xx_hal_tim.h"

#endif
