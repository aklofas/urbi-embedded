# Mandelbrot demo for STM32F429I-DISC1
#
# Standard targets:
#   make             — build build/mandelbrot.{elf,bin} (incremental)
#   make flash       — build (if needed) + openocd-flash the board
#   make debug       — flash + start openocd gdbserver + arm-none-eabi-gdb
#   make clean       — remove build/ and mandelbrot_baked.h
#   make distclean   — also rebuild liburbi.a from scratch

TARGET     := mandelbrot
BUILD      := build
OBJDIR     := $(BUILD)/obj

# Repo paths (relative to this Makefile's directory)
URBI_ROOT  := ../../..
HAL_ROOT   := ../../../../tools/stm32cube-f4
HAL_INC    := $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Inc
HAL_SRC    := $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src
BSP_DIR    := $(HAL_ROOT)/Drivers/BSP/STM32F429I-Discovery
CMP_DIR    := $(HAL_ROOT)/Drivers/BSP/Components

# Toolchain
CC         := arm-none-eabi-gcc
OBJC       := arm-none-eabi-objcopy
SIZE       := arm-none-eabi-size
GDB        := arm-none-eabi-gdb

# Target flags
MCU        := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

# Heap: 1 MB in external SDRAM at 0xD0080000 (clear of the 150 KB framebuffer
# at the SDRAM base 0xD0000000).
#
# URBI_FLOAT_TYPE=4 MUST match the URBI_FLOAT_TYPE the linked liburbi.a was
# built with (cross-stm32f4 → f32 / 4 bytes).  Mismatch is silent and brutal:
# UValue.v.f changes size (4 vs 8 bytes), application-side urbi_make_float
# writes the wrong layout, and v.f reads back as 0.0 in the runtime.
# Diagnosed at v0.8.2 hardware bring-up via disasm of port_gyro.c showing
# __aeabi_f2d + strd (8-byte store) where the runtime expected vstr.f32.
DEFS       := -DSTM32F429xx -DUSE_HAL_DRIVER \
              -DURBI_FLOAT_TYPE=4 \
              -DURBI_HEAP_BYTES=1048576UL \
              -DURBI_HEAP_EXTERNAL_ADDR=0xD0080000UL

INC        := -I. -Istubs \
              -I$(URBI_ROOT)/include \
              -I$(URBI_ROOT)/src \
              -I$(URBI_ROOT)/components/stm32f4-hal-baremetal/include \
              -I$(HAL_INC) \
              -I$(HAL_ROOT)/Drivers/CMSIS/Device/ST/STM32F4xx/Include \
              -I$(HAL_ROOT)/Drivers/CMSIS/Include \
              -I$(BSP_DIR) \
              -I$(CMP_DIR)/ili9341 \
              -I$(CMP_DIR)/l3gd20

CFLAGS     := -std=c99 -Wall -Wextra -Os -g $(MCU) -ffreestanding \
              -ffunction-sections -fdata-sections -MMD -MP $(DEFS) $(INC)
ASFLAGS    := $(MCU)
LDSCRIPT   := STM32F429ZITx_FLASH.ld
LDFLAGS    := $(MCU) -T$(LDSCRIPT) -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map \
              -ffreestanding -nostartfiles -nodefaultlibs -lgcc

# --- Sources --------------------------------------------------------------

APP_C      := main.c stm32f4xx_it.c stubs/libc_stubs.c
PORT_C     := $(wildcard $(URBI_ROOT)/components/stm32f4-hal-baremetal/src/port/*.c)
HAL_C      := $(addprefix $(HAL_SRC)/, \
                stm32f4xx_hal.c stm32f4xx_hal_cortex.c \
                stm32f4xx_hal_dma.c stm32f4xx_hal_dma2d.c \
                stm32f4xx_hal_flash.c stm32f4xx_hal_flash_ex.c \
                stm32f4xx_hal_gpio.c \
                stm32f4xx_hal_i2c.c stm32f4xx_hal_i2c_ex.c \
                stm32f4xx_hal_ltdc.c \
                stm32f4xx_hal_pwr.c stm32f4xx_hal_pwr_ex.c \
                stm32f4xx_hal_rcc.c stm32f4xx_hal_rcc_ex.c \
                stm32f4xx_hal_sdram.c stm32f4xx_hal_spi.c \
                stm32f4xx_hal_tim.c stm32f4xx_hal_tim_ex.c \
                stm32f4xx_hal_uart.c \
                stm32f4xx_ll_fmc.c stm32f4xx_ll_fsmc.c)
BSP_C      := $(addprefix $(BSP_DIR)/, \
                stm32f429i_discovery.c stm32f429i_discovery_lcd.c \
                stm32f429i_discovery_sdram.c stm32f429i_discovery_gyroscope.c) \
              $(CMP_DIR)/ili9341/ili9341.c \
              $(CMP_DIR)/l3gd20/l3gd20.c
SYS_C      := $(HAL_ROOT)/Drivers/CMSIS/Device/ST/STM32F4xx/Source/Templates/system_stm32f4xx.c
STARTUP_S  := $(HAL_ROOT)/Drivers/CMSIS/Device/ST/STM32F4xx/Source/Templates/gcc/startup_stm32f429xx.s

ALL_C      := $(APP_C) $(PORT_C) $(HAL_C) $(BSP_C) $(SYS_C)
ALL_S      := $(STARTUP_S)

URBI_LIB   := $(URBI_ROOT)/build/arm-cortex-m4/liburbi.a

# --- Object files: one per source under $(OBJDIR)/ using basename only ---
# Assumes basenames are unique across the full source set (HAL has no
# duplicate filenames; checked at v0.8.2 ship).  If a future port adds two
# files with the same basename, this will silently overwrite — extend the
# obj_path encoding then.

OBJ_C      := $(foreach s,$(ALL_C),$(OBJDIR)/$(notdir $(s)).o)
OBJ_S      := $(foreach s,$(ALL_S),$(OBJDIR)/$(notdir $(s)).o)
OBJ        := $(OBJ_C) $(OBJ_S)

DEP        := $(OBJ_C:.o=.d)
-include $(DEP)

# --- Default + phony targets ---------------------------------------------

.DEFAULT_GOAL := all
.PHONY: all flash debug clean distclean

all: $(BUILD)/$(TARGET).bin

flash: $(BUILD)/$(TARGET).bin
	openocd -f board/stm32f429discovery.cfg \
	        -c "program $< 0x08000000 verify reset exit"

debug: $(BUILD)/$(TARGET).elf
	openocd -f board/stm32f429discovery.cfg &
	sleep 1
	$(GDB) $< -ex "target remote :3333"

clean:
	rm -rf $(BUILD) mandelbrot_baked.h

distclean: clean
	$(MAKE) -C $(URBI_ROOT) clean

# --- Link + binary -------------------------------------------------------

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	@$(OBJC) -O binary $< $@
	@echo "  BIN    $@"

$(BUILD)/$(TARGET).elf: $(OBJ) $(URBI_LIB) | $(BUILD)
	@$(CC) $(LDFLAGS) $(OBJ) $(URBI_LIB) -o $@
	@echo "  LD     $@"
	@$(SIZE) $@

# --- Per-source compile rules via foreach + eval -------------------------

# define a rule per .c source so make knows which path on disk to compile
define _c_rule
$$(OBJDIR)/$$(notdir $(1)).o: $(1) | $$(OBJDIR)
	@echo "  CC     $$<"
	@$$(CC) $$(CFLAGS) -c $$< -o $$@
endef
$(foreach s,$(ALL_C),$(eval $(call _c_rule,$(s))))

# main.c has an extra dep on the generated header
$(OBJDIR)/main.c.o: mandelbrot_baked.h

# .s assembly sources
define _s_rule
$$(OBJDIR)/$$(notdir $(1)).o: $(1) | $$(OBJDIR)
	@echo "  AS     $$<"
	@$$(CC) $$(ASFLAGS) -c $$< -o $$@
endef
$(foreach s,$(ALL_S),$(eval $(call _s_rule,$(s))))

# --- Generated files + dirs ---------------------------------------------

# f4 = URBI_FLOAT_TYPE=4 (f32) bake tool — required because cross-stm32f4
# uses f32, and the default tools/urbi-compile-stdlib bakes f64 (would
# trigger ULOAD_FLAVOR_MISMATCH on byte 13 at runtime).
BAKE_TOOL := $(URBI_ROOT)/tools/urbi-compile-stdlib-f4

mandelbrot_baked.h: mandelbrot.u $(BAKE_TOOL)
	@echo "  BAKE   $@ (f32)"
	@$(BAKE_TOOL) --to-header -i $< -o $@ --symbol mandelbrot_bytecode

$(URBI_LIB):
	@echo "  MAKE   liburbi.a (cross-stm32f4)"
	@$(MAKE) -C $(URBI_ROOT) cross-stm32f4

$(BAKE_TOOL):
	@echo "  MAKE   $(notdir $@)"
	@$(MAKE) -C $(URBI_ROOT) tools/urbi-compile-stdlib-f4

$(BUILD) $(OBJDIR):
	@mkdir -p $@
