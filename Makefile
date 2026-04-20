SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

TEST_SRC := $(wildcard tests/unit/test_*.c) tests/unit/runner.c
TEST_OBJ := $(TEST_SRC:.c=.o)

CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -Os
CPPFLAGS += -Isrc -Itests/unit

all: liburbi.a

liburbi.a: $(OBJ)
	$(AR) rcs $@ $^

test: liburbi.a $(TEST_OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o tests/unit/runner $(TEST_OBJ) liburbi.a
	./tests/unit/runner

test-asan: clean
	$(MAKE) CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=address -fno-omit-frame-pointer" test

test-ubsan: clean
	$(MAKE) CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=undefined -fno-omit-frame-pointer" test

test-debug: clean
	$(MAKE) CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O0 -g" test

# Cross-compile sanity (builds liburbi.a only; no test runner)
cross-arm: clean
	$(MAKE) CC=arm-none-eabi-gcc \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os -mcpu=cortex-m7 -mthumb -ffreestanding" \
		AR=arm-none-eabi-ar \
		liburbi.a

cross-riscv: clean
	$(MAKE) CC=riscv64-unknown-elf-gcc \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os -march=rv32imc -mabi=ilp32 -ffreestanding" \
		AR=riscv64-unknown-elf-ar \
		liburbi.a

# Compilation database for clangd / CLion / VS Code indexing.
# Generated on demand; gitignored. Re-run after changing CFLAGS/CPPFLAGS or
# adding/removing source files.
compile_commands.json:
	@printf '[\n' > $@
	@first=1; for f in $(SRC) $(TEST_SRC); do \
		if [ $$first -eq 0 ]; then printf ',\n' >> $@; fi; \
		first=0; \
		printf '  {"directory": "%s", "file": "%s/%s", "command": "%s %s %s -c %s"}' \
			"$$PWD" "$$PWD" "$$f" "$(CC)" "$(CFLAGS)" "$(CPPFLAGS)" "$$f" >> $@; \
	done
	@printf '\n]\n' >> $@

clean:
	rm -f $(OBJ) $(TEST_OBJ) liburbi.a tests/unit/runner compile_commands.json

.PHONY: all test test-asan test-ubsan test-debug cross-arm cross-riscv clean compile_commands.json
