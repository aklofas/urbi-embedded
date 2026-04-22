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

# Static analysis — clang-tidy gating via run-clang-tidy.
# Fails on any clang-tidy warning (-warnings-as-errors='*').
# Check list is configured in .clang-tidy; CLI flag promotes warnings to errors.
tidy: compile_commands.json
	run-clang-tidy -p . -j $$(nproc) -warnings-as-errors='*' -quiet $(SRC)

# Local convenience: run clang-tidy with --fix.  Not invoked by CI.
tidy-fix: compile_commands.json
	run-clang-tidy -p . -j $$(nproc) -fix -format -style=file -quiet $(SRC)

# Static analysis — cppcheck (advisory).
# Different engine from clang-tidy; catches value-flow, UAF, null-deref
# that clang-tidy's AST-level checks miss.  Exits 0 regardless of
# warnings — promote to gating via a separate commit after the noise
# floor is known.
cppcheck: compile_commands.json
	cppcheck --project=compile_commands.json \
	         --std=c99 \
	         --enable=warning,style,performance,portability \
	         --suppress=missingIncludeSystem \
	         --inline-suppr \
	         --quiet

# Static analysis — GCC -fanalyzer (advisory).
# Dedicated build variant so the 20% compile-time penalty only applies
# when explicitly requested.  Diagnostics go to stderr during compile;
# the resulting build/host-analyzer/liburbi.a is a valid archive
# (-fanalyzer is diagnostic-only, doesn't change codegen).
analyzer:
	$(MAKE) TARGET=host-analyzer \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os -fanalyzer" \
		all

# Aggregate: gating tidy, advisory cppcheck, advisory analyzer.
# CI invokes this as one step per-target so failures clearly name
# which tool caught the issue.
lint: tidy cppcheck analyzer

clean:
	rm -rf build compile_commands.json

.PHONY: all test test-asan test-ubsan test-debug cross-arm cross-riscv clean compile_commands.json tidy tidy-fix cppcheck analyzer lint
