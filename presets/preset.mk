# A cross preset names the toolchain and the CPU; everything else is the
# ordinary build.  Included by the top-level Makefile when CROSS_PRESET is
# set on a recursive make.  A missing toolchain is a hard error here, not
# a skip: a gate that silently passes on a box without the compiler has
# let two releases ship with a leak before.
include presets/$(CROSS_PRESET).mk

ifeq ($(shell command -v $(CROSS_CC) 2>/dev/null),)
  $(error cross preset $(CROSS_PRESET): compiler $(CROSS_CC) is not on PATH)
endif

CC := $(CROSS_CC)
AR := $(CROSS_AR)
URBI_FREESTANDING_FLAG := $(if $(filter 1,$(URBI_HOSTED)),,-ffreestanding)
CFLAGS := -std=c99 -Wall -Wextra -Wpedantic -Os $(CROSS_CPUFLAGS) $(URBI_FREESTANDING_FLAG)
