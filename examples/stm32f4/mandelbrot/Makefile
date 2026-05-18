# Mandelbrot demo for STM32F429I-DISC1
# Build: make
# Flash: make flash  (requires openocd)

TARGET   = mandelbrot
BUILD    = build

# Repo paths (relative to this Makefile's directory)
URBI_ROOT  := ../../..
HAL_ROOT   := ../../../../tools/stm32cube-f4

# Toolchain
CC   = arm-none-eabi-gcc
AS   = arm-none-eabi-as
LD   = arm-none-eabi-gcc
OBJC = arm-none-eabi-objcopy
SIZE = arm-none-eabi-size

# Target flags
MCU = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

# Defines
DEFS = -DSTM32F429xx -DUSE_HAL_DRIVER

# Include paths
INC = -I. \
      -Istubs \
      -I$(URBI_ROOT)/include \
      -I$(URBI_ROOT)/src \
      -I$(URBI_ROOT)/components/stm32f4-hal-baremetal/include \
      -I$(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Inc \
      -I$(HAL_ROOT)/Drivers/CMSIS/Device/ST/STM32F4xx/Include \
      -I$(HAL_ROOT)/Drivers/CMSIS/Include \
      -I$(HAL_ROOT)/Drivers/BSP/STM32F429I-Discovery \
      -I$(HAL_ROOT)/Drivers/BSP/Components/ili9341 \
      -I$(HAL_ROOT)/Drivers/BSP/Components/l3gd20

CFLAGS = -std=c99 -Wall -Wextra -Os $(MCU) -ffunction-sections -fdata-sections \
         -ffreestanding $(DEFS) $(INC) -g

LDSCRIPT = STM32F429ZITx_FLASH.ld
LDFLAGS  = $(MCU) -T$(LDSCRIPT) -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map \
           -ffreestanding -nostartfiles -nodefaultlibs -lgcc

# Sources
APP_SRC = main.c stm32f4xx_it.c stubs/libc_stubs.c

PORT_SRC = $(wildcard $(URBI_ROOT)/components/stm32f4-hal-baremetal/src/port/*.c)

HAL_SRC = $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_cortex.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_gpio.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc_ex.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr_ex.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ex.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_ltdc.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_sdram.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_spi.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_fmc.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_fsmc.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim_ex.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_i2c.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_i2c_ex.c \
          $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma2d.c

BSP_SRC = $(HAL_ROOT)/Drivers/BSP/STM32F429I-Discovery/stm32f429i_discovery.c \
          $(HAL_ROOT)/Drivers/BSP/STM32F429I-Discovery/stm32f429i_discovery_lcd.c \
          $(HAL_ROOT)/Drivers/BSP/STM32F429I-Discovery/stm32f429i_discovery_sdram.c \
          $(HAL_ROOT)/Drivers/BSP/STM32F429I-Discovery/stm32f429i_discovery_gyroscope.c \
          $(HAL_ROOT)/Drivers/BSP/Components/ili9341/ili9341.c \
          $(HAL_ROOT)/Drivers/BSP/Components/l3gd20/l3gd20.c

STARTUP_SRC = $(HAL_ROOT)/Drivers/CMSIS/Device/ST/STM32F4xx/Source/Templates/gcc/startup_stm32f429xx.s
SYSINIT_SRC = $(HAL_ROOT)/Drivers/CMSIS/Device/ST/STM32F4xx/Source/Templates/system_stm32f4xx.c

URBI_LIB = $(URBI_ROOT)/build/arm-cortex-m4/liburbi.a

$(BUILD)/$(TARGET).elf: $(APP_SRC) $(PORT_SRC) $(HAL_SRC) $(BSP_SRC) $(STARTUP_SRC) $(SYSINIT_SRC) $(URBI_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@
	$(SIZE) $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJC) -O binary $< $@

$(BUILD):
	mkdir -p $@

$(URBI_LIB):
	$(MAKE) -C $(URBI_ROOT) cross-stm32f4

# mandelbrot_baked.h — generated from mandelbrot.u via tools/urbi-compile-stdlib
# FIXME(T21): adjust flags once the exact --to-header invocation is settled in Task 20.
mandelbrot_baked.h: mandelbrot.u $(URBI_ROOT)/tools/urbi-compile-stdlib
	$(URBI_ROOT)/tools/urbi-compile-stdlib --to-header -i $< -o $@ --symbol mandelbrot_bytecode

$(URBI_ROOT)/tools/urbi-compile-stdlib:
	$(MAKE) -C $(URBI_ROOT) tools/urbi-compile-stdlib

.PHONY: all clean flash debug
all: $(BUILD)/$(TARGET).bin

flash: $(BUILD)/$(TARGET).bin
	openocd -f board/stm32f429discovery.cfg \
	        -c "program $< 0x08000000 verify reset exit"

debug:
	openocd -f board/stm32f429discovery.cfg &
	sleep 1
	arm-none-eabi-gdb $(BUILD)/$(TARGET).elf -ex "target remote :3333"

clean:
	rm -rf $(BUILD) mandelbrot_baked.h
