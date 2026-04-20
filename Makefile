SRC := $(wildcard src/*.c)
TEST_SRC := $(wildcard tests/unit/test_*.c) tests/unit/runner.c

TARGET ?= host
BUILDDIR := build/$(TARGET)

OBJ := $(patsubst src/%.c,$(BUILDDIR)/src/%.o,$(SRC))
TEST_OBJ := $(patsubst tests/unit/%.c,$(BUILDDIR)/tests/unit/%.o,$(TEST_SRC))
LIB := $(BUILDDIR)/liburbi.a
RUNNER := $(BUILDDIR)/tests/unit/runner

CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -Os
CPPFLAGS += -Isrc -Itests/unit

all: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

$(BUILDDIR)/src/%.o: src/%.c | $(BUILDDIR)/src
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILDDIR)/tests/unit/%.o: tests/unit/%.c | $(BUILDDIR)/tests/unit
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILDDIR)/src $(BUILDDIR)/tests/unit:
	@mkdir -p $@

test: $(LIB) $(TEST_OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $(RUNNER) $(TEST_OBJ) $(LIB)
	$(RUNNER)

test-debug:
	$(MAKE) TARGET=host-debug \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O0 -g" \
		test

test-asan:
	$(MAKE) TARGET=host-asan \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=address -fno-omit-frame-pointer" \
		test

test-ubsan:
	$(MAKE) TARGET=host-ubsan \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=undefined -fno-omit-frame-pointer" \
		test

# Cross-compile sanity (builds liburbi.a only; no test runner).
cross-arm:
	$(MAKE) TARGET=arm-cortex-m7 \
		CC=arm-none-eabi-gcc \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os -mcpu=cortex-m7 -mthumb -ffreestanding" \
		AR=arm-none-eabi-ar \
		all

cross-riscv:
	$(MAKE) TARGET=riscv-rv32imc \
		CC=riscv64-unknown-elf-gcc \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os -march=rv32imc -mabi=ilp32 -ffreestanding" \
		AR=riscv64-unknown-elf-ar \
		all

# Compilation database for clangd / CLion / VS Code indexing.
# Generated on demand; gitignored. Re-run after changing CFLAGS/CPPFLAGS or
# adding/removing source files.
compile_commands.json:
	@printf '[\n' > $@
	@first=1; for f in $(SRC) $(TEST_SRC); do \
		if [ $$first -eq 0 ]; then printf ',\n' >> $@; fi; \
		first=0; \
		printf '  {"directory": "%s", "file": "%s/%s", "command": "%s %s %s -c -o %s/%s/%s %s"}' \
			"$$PWD" "$$PWD" "$$f" "$(CC)" "$(CFLAGS)" "$(CPPFLAGS)" \
			"$$PWD" "$(BUILDDIR)" "$${f%.c}.o" "$$f" >> $@; \
	done
	@printf '\n]\n' >> $@

clean:
	rm -rf build compile_commands.json

.PHONY: all test test-asan test-ubsan test-debug cross-arm cross-riscv clean compile_commands.json
