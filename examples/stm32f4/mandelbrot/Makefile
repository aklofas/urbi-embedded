# Mandelbrot demo for STM32F429I-DISC1
#
# Standard targets:
#   make             — build build/mandelbrot.{elf,bin} (incremental)
#   make flash       — build (if needed) + flash via STM32_Programmer_CLI
#   make flash-stlink — build (if needed) + flash via st-flash
#   make debug       — flash + start openocd gdbserver + arm-none-eabi-gdb
#   make clean       — remove build/ and mandelbrot_baked.h
#   make distclean   — also rebuild liburbi.a from scratch

TARGET     := mandelbrot
BUILD      := build
OBJDIR     := $(BUILD)/obj

# Repo paths (relative to this Makefile's directory)
URBI_ROOT  := ../../..
HAL_ROOT   ?= $(abspath ../../../../tools/stm32cube-f4)
HAL_INC    := $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Inc
HAL_SRC    := $(HAL_ROOT)/Drivers/STM32F4xx_HAL_Driver/Src
BSP_DIR    := $(HAL_ROOT)/Drivers/BSP/STM32F429I-Discovery
CMP_DIR    := $(HAL_ROOT)/Drivers/BSP/Components

# Toolchain: CC is normally handed down resolved by the top-level
# `stm32f4-mandelbrot` target; a direct `make` here falls back to PATH.
# OBJC/SIZE are derived from CC's own directory and prefix so the whole
# toolchain comes from one place even when CC is an absolute path.
CC         ?= arm-none-eabi-gcc
TOOLDIR    := $(dir $(shell command -v $(CC)))
OBJC       := $(TOOLDIR)arm-none-eabi-objcopy
SIZE       := $(TOOLDIR)arm-none-eabi-size
GDB        := arm-none-eabi-gdb

# Target flags
MCU        := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

# Heap: 128 KB internal SRAM arena (the component's default).
# External heap: add -DURBI_HEAP_EXTERNAL_ADDR=0xD0080000UL -DURBI_HEAP_BYTES=1048576UL
# to DEFS (the SDRAM controller is initialised before the first allocation
# either way).
DEFS       := -DSTM32F429xx -DUSE_HAL_DRIVER \
              -DURBI_BYTECODE_ONLY=1 \
              -DURBI_HEAP_BYTES=131072UL

INC        := -I. -Istubs \
              -I$(URBI_ROOT)/include \
              -I$(URBI_ROOT)/components/stm32f4-hal-baremetal/include \
              -I$(HAL_INC) \
              -I$(HAL_ROOT)/Drivers/CMSIS/Device/ST/STM32F4xx/Include \
              -I$(HAL_ROOT)/Drivers/CMSIS/Include \
              -I$(BSP_DIR) \
              -I$(CMP_DIR)/ili9341 \
              -I$(CMP_DIR)/l3gd20

# The project's own files compile strict; HAL/BSP/CMSIS sources only get
# the plain warnings (vendor code, not ours to make -Wpedantic-clean).
COMMON_CFLAGS := -std=c99 -Os -g $(MCU) -ffreestanding \
              -ffunction-sections -fdata-sections -MMD -MP $(DEFS) $(INC)
APP_CFLAGS := $(COMMON_CFLAGS) -Wall -Wextra -Wpedantic -Werror
HAL_CFLAGS := $(COMMON_CFLAGS) -Wall -Wextra
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

# APP_C last: under a serial build, a failure in our own code (e.g. a
# retired API main.c hasn't caught up with yet) still lets every other
# source get a compile attempt instead of stopping the run on the first
# prerequisite in the list.
ALL_C      := $(PORT_C) $(HAL_C) $(BSP_C) $(SYS_C) $(APP_C)
ALL_S      := $(STARTUP_S)

URBI_LIB   := $(URBI_ROOT)/build/arm-cortex-m4f-bytecode-only/liburbi.a

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
.PHONY: all flash flash-stlink debug clean distclean

all: check-hal-root $(BUILD)/$(TARGET).bin

# Fails loudly before anything is built if HAL_ROOT doesn't point at a
# real STM32CubeF4 checkout, rather than letting the compiler report a
# missing-header error for every HAL/BSP source in turn.
.PHONY: check-hal-root
check-hal-root:
	@test -f $(HAL_INC)/stm32f4xx_hal.h || { echo "mandelbrot: no STM32CubeF4 at $(HAL_ROOT) (set HAL_ROOT)"; exit 1; }

flash: $(BUILD)/$(TARGET).bin
	STM32_Programmer_CLI -c port=SWD -w $(BUILD)/$(TARGET).bin 0x08000000 -rst

flash-stlink: $(BUILD)/$(TARGET).bin
	st-flash --reset write $(BUILD)/$(TARGET).bin 0x08000000

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

# define a rule per .c source so make knows which path on disk to compile;
# the project's own files are listed in APP_C/PORT_C and get the strict
# flags, everything else (HAL/BSP/CMSIS) gets the lenient ones.
define _c_rule
$$(OBJDIR)/$$(notdir $(1)).o: $(1) | $$(OBJDIR)
	@echo "  CC     $$<"
	@$$(CC) $(if $(filter $(1),$(APP_C) $(PORT_C)),$$(APP_CFLAGS),$$(HAL_CFLAGS)) -c $$< -o $$@
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

mandelbrot_baked.h: mandelbrot.u $(URBI_ROOT)/tools/bake-header.sh $(URBI_ROOT)/build/host/urbi
	@echo "  BAKE   $@"
	@sh $(URBI_ROOT)/tools/bake-header.sh $(URBI_ROOT)/build/host/urbi mandelbrot.u $@ mandelbrot_wire

$(URBI_ROOT)/build/host/urbi:
	@echo "  MAKE   urbi"
	@$(MAKE) -C $(URBI_ROOT) urbi-bin

$(URBI_LIB):
	@echo "  MAKE   liburbi.a (cross-arm-cortex-m4f-bytecode-only)"
	@$(MAKE) -C $(URBI_ROOT) cross-arm-cortex-m4f-bytecode-only

$(BUILD) $(OBJDIR):
	@mkdir -p $@
