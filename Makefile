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
RUNNER_WRAPPER ?=

all: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

$(BUILDDIR)/src/%.o: src/%.c | $(BUILDDIR)/src
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILDDIR)/tests/unit/%.o: tests/unit/%.c | $(BUILDDIR)/tests/unit
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILDDIR)/src $(BUILDDIR)/tests/unit:
	@mkdir -p $@

# --- REPL binary --------------------------------------------------------
#
# urbi — the REPL binary.  Builds from tools/urbi.c + vendored linenoise
# against the library archive.  Never built on cross-compile targets
# (tools/ depends on POSIX stdio / termios).

TOOLS_SRC := tools/urbi.c tools/linenoise.c

$(BUILDDIR)/tools:
	@mkdir -p $@

# linenoise is vendored third-party code; compile it separately with
# -D_POSIX_C_SOURCE + -w to suppress upstream strict-C99 warnings
# (variadic macro and strcasecmp declaration).  urbi.c is compiled
# with the standard CFLAGS.
$(BUILDDIR)/tools/linenoise.o: tools/linenoise.c | $(BUILDDIR)/tools
	$(CC) -std=c99 -Os -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 \
	    -w -Itools -c -o $@ $<

$(BUILDDIR)/tools/urbi.o: tools/urbi.c | $(BUILDDIR)/tools
	$(CC) $(CFLAGS) $(CPPFLAGS) -Itools -c -o $@ $<

$(BUILDDIR)/urbi: $(BUILDDIR)/tools/urbi.o $(BUILDDIR)/tools/linenoise.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $(BUILDDIR)/tools/urbi.o $(BUILDDIR)/tools/linenoise.o $(LIB)

urbi-bin: $(BUILDDIR)/urbi

# --- Integration tests --------------------------------------------------
#
# test-integration runs the REPL shell harness against the built binary.
# Folded into the existing `test` aggregate so it runs under every
# sanitizer variant automatically. The shell script itself is NOT wrapped
# by $(RUNNER_WRAPPER) because dash's own "still-reachable" blocks break
# valgrind; the urbi binary is memory-clean when invoked directly.

test-integration: $(BUILDDIR)/urbi
	tests/integration/repl_smoke.sh $(BUILDDIR)/urbi

# --- .chk conformance fixtures -----------------------------------------
#
# test-chk iterates all tests/chk/**/*.chk against the built urbi binary
# via tests/integration/run_chk.sh. One REPL session per fixture. Folded
# into `test` alongside test-integration so every sanitizer variant
# runs the fixtures automatically. Not valgrind-wrapped (same rationale
# as test-integration — urbi itself is memory-clean, and wrapping the
# sh+awk+sed pipeline adds noise, not signal).

test-chk: $(BUILDDIR)/urbi
	@set -e; \
	count=0; \
	for f in $$(find tests/chk -name '*.chk' 2>/dev/null | sort); do \
	    count=$$((count + 1)); \
	    tests/integration/run_chk.sh $(BUILDDIR)/urbi "$$f"; \
	done; \
	echo "$$count chk fixture(s) passed"

test: $(LIB) $(TEST_OBJ) test-integration test-chk
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $(RUNNER) $(TEST_OBJ) $(LIB)
	$(RUNNER_WRAPPER) $(RUNNER)

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

# test-switch — builds with -DURBI_VM_FORCE_SWITCH=1 to force the portable
# switch-based VM dispatch path even on GCC/Clang. Keeps both dispatch
# paths compiling and passing continuously; see
# docs/superpowers/specs/2026-04-23-urbi-embedded-vm-design.md §2.4.
test-switch:
	$(MAKE) TARGET=host-switch \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os -DURBI_VM_FORCE_SWITCH=1" \
		test

# Valgrind memcheck — runs the test suite under valgrind's memcheck tool.
# Catches uninitialized reads, heap corruption, leaks.  Complements ASan:
# memcheck's bit-precise tracking catches uninit reads that ASan misses.
# Uses -O0 -g for readable stack traces; --error-exitcode=1 makes any
# finding fail the build.
test-valgrind: valgrind-tools
	$(MAKE) TARGET=host-valgrind \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O0 -g" \
		RUNNER_WRAPPER="valgrind --tool=memcheck --error-exitcode=1 --leak-check=full --track-origins=yes --show-leak-kinds=all -q" \
		test

valgrind-tools:
	@command -v valgrind >/dev/null 2>&1 || { \
	    echo "error: valgrind not found in PATH"; \
	    echo "install: sudo apt-get install -y valgrind"; \
	    exit 1; \
	}

# --- Release test aggregate --------------------------------------------
#
# releasetest runs every host-side gate the CI matrix runs, in sequence.
# Cross-compile jobs (cross-arm, cross-riscv) are excluded — their
# toolchains are not universally installable. CI remains authoritative
# for cross-compile verification; this target is for local pre-release
# confidence that a branch will pass CI end-to-end.
#
# Runtime: ~3-5 minutes on typical development hardware (dominated by
# test-valgrind). Invoked manually before tagging a release, or before
# pushing a branch that touches multiple subsystems.
#
# Uses recursive $(MAKE) rather than prerequisite-style target chaining
# so that a failure identifies exactly which gate broke, and so the
# sanitizer variants run their own nested rebuild rather than sharing
# object files with the previous target.

releasetest:
	@echo "=== releasetest: unit + sanitizer matrix ==="
	$(MAKE) test
	$(MAKE) test-asan
	$(MAKE) test-ubsan
	$(MAKE) test-debug
	$(MAKE) test-switch
	@echo "=== releasetest: valgrind memcheck ==="
	$(MAKE) test-valgrind
	@echo "=== releasetest: static analysis ==="
	$(MAKE) lint
	@echo "=== releasetest: documentation ==="
	$(MAKE) docs-check
	@echo "=== releasetest: coverage ==="
	$(MAKE) coverage
	@echo "=== releasetest: all gates passed ==="

# libFuzzer — clang-specific (uses libclang_rt.fuzzer, ships with clang's
# compiler-rt).  Builds each harness as a standalone binary against the
# full src/ tree; no .a dependency because libFuzzer needs the sanitizer
# runtimes linked in.  Local-only (no CI); see docs/internals/test-harness.md
# for time-budget guidance.
FUZZ_BUILDDIR := build/host-fuzz
FUZZ_CC       ?= clang
FUZZ_CFLAGS   := -std=c99 -Wall -Wextra -Wpedantic -O1 -g \
                 -fsanitize=fuzzer,address,undefined \
                 -fno-omit-frame-pointer

$(FUZZ_BUILDDIR):
	@mkdir -p $@

$(FUZZ_BUILDDIR)/fuzz_lex: tests/fuzz/fuzz_lex.c $(SRC) | $(FUZZ_BUILDDIR)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(CPPFLAGS) -o $@ $(SRC) tests/fuzz/fuzz_lex.c

$(FUZZ_BUILDDIR)/fuzz_parse: tests/fuzz/fuzz_parse.c $(SRC) | $(FUZZ_BUILDDIR)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(CPPFLAGS) -o $@ $(SRC) tests/fuzz/fuzz_parse.c

$(FUZZ_BUILDDIR)/fuzz_vm: tests/fuzz/fuzz_vm.c $(SRC) | $(FUZZ_BUILDDIR)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(CPPFLAGS) -o $@ $(SRC) tests/fuzz/fuzz_vm.c

fuzz-lex: fuzz-tools $(FUZZ_BUILDDIR)/fuzz_lex
	@echo "running fuzz_lex (Ctrl-C to stop; use -runs=N for bounded)"
	$(FUZZ_BUILDDIR)/fuzz_lex

fuzz-parse: fuzz-tools $(FUZZ_BUILDDIR)/fuzz_parse
	@echo "running fuzz_parse (Ctrl-C to stop; use -runs=N for bounded)"
	$(FUZZ_BUILDDIR)/fuzz_parse

fuzz-vm: fuzz-tools $(FUZZ_BUILDDIR)/fuzz_vm
	@echo "running fuzz_vm (Ctrl-C to stop; use -runs=N for bounded)"
	$(FUZZ_BUILDDIR)/fuzz_vm

fuzz-build: fuzz-tools $(FUZZ_BUILDDIR)/fuzz_lex $(FUZZ_BUILDDIR)/fuzz_parse $(FUZZ_BUILDDIR)/fuzz_vm

fuzz-tools:
	@command -v $(FUZZ_CC) >/dev/null 2>&1 || { \
	    echo "error: $(FUZZ_CC) not found in PATH"; \
	    echo "install: sudo apt-get install -y clang libclang-rt-18-dev"; \
	    exit 1; \
	}

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
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os -march=rv32imc -mabi=ilp32 -ffreestanding -DURBI_FLOAT_TYPE=4" \
		AR=riscv64-unknown-elf-ar \
		all

# Compilation database for clangd / CLion / VS Code indexing.
# Generated on demand; gitignored. Re-run after changing CFLAGS/CPPFLAGS or
# adding/removing source files.
compile_commands.json:
	@printf '[\n' > $@
	@first=1; for f in $(SRC) $(TEST_SRC) tools/urbi.c tools/linenoise.c; do \
		if [ $$first -eq 0 ]; then printf ',\n' >> $@; fi; \
		first=0; \
		printf '  {"directory": "%s", "file": "%s/%s", "command": "%s %s %s -Itools -c -o %s/%s/%s %s"}' \
			"$$PWD" "$$PWD" "$$f" "$(CC)" "$(CFLAGS)" "$(CPPFLAGS)" \
			"$$PWD" "$(BUILDDIR)" "$${f%.c}.o" "$$f" >> $@; \
	done
	@printf '\n]\n' >> $@

# Static analysis — clang-tidy gating via run-clang-tidy.
# Fails on any clang-tidy warning (-warnings-as-errors='*').
# Check list is configured in .clang-tidy; CLI flag promotes warnings to errors.
tidy: compile_commands.json
	run-clang-tidy -p . -j $$(nproc) -warnings-as-errors='*' -quiet $(SRC) tools/urbi.c

# Local convenience: run clang-tidy with --fix.  Not invoked by CI.
tidy-fix: compile_commands.json
	run-clang-tidy -p . -j $$(nproc) -fix -format -style=file -quiet $(SRC) tools/urbi.c

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
# -Wpedantic is intentionally omitted here: the label-as-value
# computed-goto dispatch in uvm.c would otherwise emit 16 pedantic
# warnings per build. Noise, not signal — pedantic is enforced on the
# regular `test` target instead. The -fanalyzer diagnostics are the
# whole point of this build variant.
analyzer:
	$(MAKE) TARGET=host-analyzer \
		CFLAGS="-std=c99 -Wall -Wextra -Os -fanalyzer" \
		all

# Coverage — instruments the test runner with gcov, runs it, and produces
# a gcovr summary on stdout + browsable HTML report at
# build/host-coverage/report.html.  Source filter restricts reports to
# src/ (not tests/unit/).  Requires gcovr in PATH; clobbers prior .gcda
# so repeated runs produce clean counts.
coverage: coverage-tools
	rm -f build/host-coverage/src/*.gcda build/host-coverage/tests/unit/*.gcda
	$(MAKE) TARGET=host-coverage \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O0 -g --coverage" \
		test
	gcovr --root . \
	      --object-directory build/host-coverage \
	      --filter 'src/' \
	      --txt \
	      --html-details build/host-coverage/report.html
	@echo ""
	@echo "HTML report: build/host-coverage/report.html"

coverage-tools:
	@command -v gcovr >/dev/null 2>&1 || { \
	    echo "error: gcovr not found in PATH"; \
	    echo "install: sudo apt-get install -y gcovr  # or: pip install --user gcovr"; \
	    exit 1; \
	}

# Aggregate: gating audit-globals, tidy, advisory cppcheck, advisory analyzer.
# CI invokes this as one step per-target so failures clearly name
# which tool caught the issue.
lint: audit-globals tidy cppcheck analyzer

audit-globals:
	@./tools/audit-globals.sh

clean:
	rm -rf build compile_commands.json

# ---- documentation verification ------------------------------------------
#
# docs-check runs markdown lint + intra-repo link checking over docs/ and the
# top-level README / CONTRIBUTING / CHANGELOG. Gated in CI via the docs-check
# job (see .github/workflows/ci.yml). Requires markdownlint-cli2 and
# markdown-link-check in PATH; install with:
#     npm install -g markdownlint-cli2@0.13 markdown-link-check@3.12

DOCS_LINT_TARGETS := 'docs/**/*.md' README.md CONTRIBUTING.md CHANGELOG.md

docs-check: docs-check-tools
	markdownlint-cli2 --config .markdownlint.yaml $(DOCS_LINT_TARGETS)
	@echo "--- link-check ---"
	@find docs README.md CONTRIBUTING.md CHANGELOG.md -name '*.md' -type f \
	    -exec markdown-link-check --quiet --config .markdown-link-check.json {} +

docs-check-tools:
	@command -v markdownlint-cli2 >/dev/null 2>&1 || { \
	    echo "error: markdownlint-cli2 not found in PATH"; \
	    echo "install: npm install -g markdownlint-cli2@0.13"; \
	    exit 1; \
	}
	@command -v markdown-link-check >/dev/null 2>&1 || { \
	    echo "error: markdown-link-check not found in PATH"; \
	    echo "install: npm install -g markdown-link-check@3.12"; \
	    exit 1; \
	}

.PHONY: all test test-asan test-ubsan test-debug test-switch cross-arm cross-riscv clean compile_commands.json tidy tidy-fix cppcheck analyzer lint docs-check docs-check-tools coverage coverage-tools test-valgrind valgrind-tools fuzz-lex fuzz-parse fuzz-vm fuzz-build fuzz-tools urbi-bin test-integration test-chk releasetest
