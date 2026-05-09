SRC := $(wildcard src/*.c) \
       $(wildcard src/lex/*.c) \
       $(wildcard src/parse/*.c) \
       $(wildcard src/emit/*.c) \
       $(wildcard src/vm/*.c) \
       $(wildcard src/gc/*.c) \
       $(wildcard src/sched/*.c) \
       $(wildcard src/watcher/*.c) \
       $(wildcard src/event/*.c) \
       $(wildcard src/tag/*.c) \
       $(wildcard src/changed/*.c) \
       $(wildcard src/module/*.c) \
       $(wildcard src/value/*.c) \
       $(wildcard src/runtime/*.c) \
       $(wildcard src/realm/*.c) \
       $(wildcard src/object/*.c)
TEST_SRC := $(wildcard tests/unit/test_*.c) tests/unit/runner.c \
            tests/unit/twatcher_install_helper.c \
            tests/unit/utest_e2e_helpers.c

TARGET ?= host
BUILDDIR := build/$(TARGET)

OBJ := $(patsubst src/%.c,$(BUILDDIR)/src/%.o,$(SRC))
TEST_OBJ := $(patsubst tests/unit/%.c,$(BUILDDIR)/tests/unit/%.o,$(TEST_SRC))
LIB := $(BUILDDIR)/liburbi.a
RUNNER := $(BUILDDIR)/tests/unit/runner

CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -Os
CPPFLAGS += -Iinclude -Isrc -Itests/unit
RUNNER_WRAPPER ?=

all: $(LIB)

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

$(BUILDDIR)/src/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILDDIR)/tests/unit/%.o: tests/unit/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

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
	    URBI_BUILD_PRESET=default tests/integration/run_chk.sh $(BUILDDIR)/urbi "$$f"; \
	done; \
	echo "$$count chk fixture(s) passed"

test: $(LIB) $(TEST_OBJ) test-integration test-chk
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $(RUNNER) $(TEST_OBJ) $(LIB)
	$(RUNNER_WRAPPER) $(RUNNER)

.PHONY: test-loc-cap
test-loc-cap:
	@./tests/scripts/check_loc_cap.sh

.PHONY: test-wire-format-determinism
test-wire-format-determinism: $(BUILDDIR)/urbi
	@./tests/scripts/check_wire_format_determinism.sh

test-debug:
	$(MAKE) TARGET=host-debug \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O0 -g -DURBI_DEBUG=1" \
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

# --- Determinism gate -------------------------------------------------------
#
# test-determinism builds and runs the full unit-test suite 100 times under
# each of 3 tunable presets (footprint / default / linux), verifying that
# urbi_get_determinism_checksum() returns a stable value across runs.
#
# The full runner is invoked each iteration (no per-suite filter exists in
# runner.c); at ~15-25ms per run, 300 total invocations take ~5-7 seconds.
# Any non-zero exit from the runner fails the gate with the iteration number.
#
# All three presets enable -DURBI_DEBUG=1 because the determinism checksum
# function is guarded by #ifdef URBI_DEBUG.  Distinct TARGET= values give
# each preset its own BUILDDIR so no clean step is needed between presets.

test-determinism-footprint:
	$(MAKE) TARGET=host-determinism-footprint \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -DURBI_DEBUG=1 -DURBI_CLEANUP_MAX=16 -DURBI_STRAND_BUDGET_MAX=200 -DURBI_GC_SLICE_BUDGET=2048 -DURBI_IC_ENTRIES_PER_SITE=2" \
		test
	@echo "=== Determinism gate: footprint preset (100 runs) ==="
	@for i in $$(seq 1 100); do \
	    build/host-determinism-footprint/tests/unit/runner > /dev/null \
	    || { echo "FAIL on iteration $$i (footprint preset)"; exit 1; }; \
	done
	@echo "=== Footprint preset: 100 runs PASS ==="

test-determinism-default:
	$(MAKE) TARGET=host-determinism-default \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -DURBI_DEBUG=1" \
		test
	@echo "=== Determinism gate: default preset (100 runs) ==="
	@for i in $$(seq 1 100); do \
	    build/host-determinism-default/tests/unit/runner > /dev/null \
	    || { echo "FAIL on iteration $$i (default preset)"; exit 1; }; \
	done
	@echo "=== Default preset: 100 runs PASS ==="

test-determinism-linux:
	$(MAKE) TARGET=host-determinism-linux \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -DURBI_DEBUG=1 -DURBI_GC_SLICE_BUDGET=16384 -DURBI_EVENT_RING_DEPTH=256" \
		test
	@echo "=== Determinism gate: linux preset (100 runs) ==="
	@for i in $$(seq 1 100); do \
	    build/host-determinism-linux/tests/unit/runner > /dev/null \
	    || { echo "FAIL on iteration $$i (linux preset)"; exit 1; }; \
	done
	@echo "=== Linux preset: 100 runs PASS ==="

test-determinism: test-determinism-footprint test-determinism-default test-determinism-linux
	@echo "=== Determinism gate: all 3 presets × 100 runs PASS ==="

# Valgrind memcheck — runs the test suite under valgrind's memcheck tool.
# Catches uninitialized reads, heap corruption, leaks.  Complements ASan:
# memcheck's bit-precise tracking catches uninit reads that ASan misses.
# Uses -O1 -g: -g preserves stack traces, -O1 cuts the instruction volume
# valgrind has to instrument (~20-30% faster than -O0) without harming
# diagnosis. --error-exitcode=1 makes any finding fail the build.
# --track-origins=yes and --show-leak-kinds=all are deliberately omitted —
# they roughly double runtime and are only useful when triaging a hit;
# re-enable locally for that. Default leak-check (definite+possible) is
# enough for CI gating.
#
# Sharding: set URBI_SHARD_TOTAL=N URBI_SHARD_INDEX=I in the environment
# to run only suites where (suite_index % N == I). CI uses N=4 across a
# matrix; locally, leave unset to run all suites. The Makefile does NOT
# dispatch sharded valgrind by default — empirically the wall-clock cost
# is concentrated in 1-2 specific suites, so per-suite sharding ends up
# strictly worse than running the suite once (the heavy shard alone
# exceeds the unsharded total because every shard pays valgrind
# startup + leak-summary cost, and the worst case is bottleneck-bound).
# The wall-clock win in releasetest comes from running test-valgrind
# in parallel with test-valgrind-deep + the sanitizer matrix + lint +
# coverage etc., which IS what releasetest does.
test-valgrind: valgrind-tools
	$(MAKE) TARGET=host-valgrind \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g" \
		RUNNER_WRAPPER="valgrind --tool=memcheck --error-exitcode=1 --leak-check=full -q" \
		test

# test-valgrind-deep — diagnostic counterpart to test-valgrind. Enables
# --track-origins=yes (resolves uninit-read complaints to the allocation
# site that produced the undefined byte) and --show-leak-kinds=all
# (reports indirect/reachable/suppressed leaks, not just definite/possible).
# Roughly 2× slower than test-valgrind. Intended for local triage when
# test-valgrind reports a hit and you need a usable stack trace, or for
# pre-release sweeps. NOT wired into CI — too expensive for per-push gating.
# Sharding env vars (URBI_SHARD_TOTAL/INDEX) work here too.
test-valgrind-deep: valgrind-tools
	$(MAKE) TARGET=host-valgrind-deep \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O0 -g" \
		RUNNER_WRAPPER="valgrind --tool=memcheck --error-exitcode=1 --leak-check=full --track-origins=yes --show-leak-kinds=all -q" \
		test

valgrind-tools:
	@command -v valgrind >/dev/null 2>&1 || { \
	    echo "error: valgrind not found in PATH"; \
	    echo "install: sudo apt-get install -y valgrind"; \
	    exit 1; \
	}

# T126: Full-corpus sanitizer gate (Wave 5 spec §3.9 verification G4).
# Runs every tests/chk/**/*.chk fixture under ASan + UBSan + valgrind
# memcheck (full leak-check).  Promotes from Wave-5's curated subset to a
# standing all-fixtures gate.  Solo in releasetest Phase 2 to avoid
# bandwidth contention (per project_releasetest_perf.md).
.PHONY: test-corpus-sanitize
test-corpus-sanitize:
	@$(MAKE) TARGET=host-asan \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=address -fno-omit-frame-pointer" \
		urbi-bin
	@$(MAKE) TARGET=host-ubsan \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=undefined -fno-omit-frame-pointer" \
		urbi-bin
	bash tests/integration/test_full_corpus_sanitize.sh

# --- Release test aggregate --------------------------------------------
#
# releasetest runs every host-side gate the CI matrix runs, in parallel.
# Cross-compile jobs (cross-arm, cross-riscv) are excluded — their
# toolchains are not universally installable. CI remains authoritative
# for cross-compile verification; this target is for local pre-release
# confidence that a branch will pass CI end-to-end.
#
# Runtime: ~5 minutes on a 32-core / 64 GB box (dominated by the two
# valgrind passes; sanitizer variants and analysis run alongside them).
# Invoked manually before tagging a release, or before pushing a branch
# that touches multiple subsystems.
#
# All sub-targets use disjoint $(BUILDDIR) trees ($(TARGET)=host /
# host-asan / host-ubsan / host-debug / host-switch / host-valgrind /
# host-valgrind-deep / host-coverage / host-analyzer), so concurrent
# rebuilds do not race.  The sole shared artifact is build/host/liburbi.a
# (needed by `test`, `test-stress`, and the `lint` machinery's
# compile_commands.json consumers); GNU make's dep graph builds it once
# and gates dependent rules on it.
#
# Set RELEASETEST_JOBS to override the parallelism level (default: nproc).
# Use RELEASETEST_OUTPUT=line to disable output grouping if you need to
# stream interleaved logs (default: -Otarget — each sub-target's stdout
# arrives in one block, so logs are still readable).

# Phase 1: every CPU-bound gate that doesn't compete badly with valgrind.
# Runs concurrently under -j$(RELEASETEST_JOBS).
#
# T118: test-scan-build promoted into releasetest after Phase 19 closed
# its known false-positive set ("scan-build: No bugs found." at v0.5.7).
# test-tidy-strict and test-cppcheck remain informational at v0.5.7
# (25 / 145 residual violations respectively, all non-trivial categories
# tracked in docs/urbi-embedded-backlog.md "Strict-tooling residuals" —
# bugprone-branch-clone, performance-no-int-to-ptr, and clang-analyzer-
# valist need either targeted refactors or per-site NOLINT review).
# Promote those two to hard gates after the residuals close.
RELEASETEST_PHASE1 := \
    test test-asan test-ubsan test-debug test-switch \
    lint docs-check coverage test-stress test-gc-none-build \
    test-scan-build test-wire-format-determinism
# Phase 2: valgrind, running alone after Phase 1 finishes.
# Empirically valgrind throughput collapses by 10-20× when sharing memory
# bandwidth with concurrent gcov / clang-tidy / cppcheck / fanalyzer
# (instrumented runner balloons from ~2 min solo to 40+ min under
# contention).  Phase 2 is sequential — the cumulative wall-clock with
# Phase 1 first is still substantially faster than the original 15-min
# fully-sequential design.
RELEASETEST_PHASE2 := test-valgrind test-corpus-sanitize

# test-valgrind-deep is intentionally NOT in releasetest. Per its
# docstring ("Intended for local triage when test-valgrind reports a hit
# and you need a usable stack trace") it is a triage tool, not a gate;
# its --track-origins=yes and --show-leak-kinds=all flags roughly double
# wall-clock vs the fast variant. Run it explicitly when triaging:
#   make test-valgrind-deep

RELEASETEST_JOBS   ?= $(shell nproc)
RELEASETEST_OUTPUT ?= target

releasetest:
	@echo "=== releasetest: 2-phase sweep ==="
	@echo "Phase 1 ($(words $(RELEASETEST_PHASE1)) gates, -j$(RELEASETEST_JOBS) -O$(RELEASETEST_OUTPUT)): $(RELEASETEST_PHASE1)"
	@echo "Phase 2 ($(words $(RELEASETEST_PHASE2)) gate, sequential): $(RELEASETEST_PHASE2)"
	@start_ts=$$(date +%s); \
	$(MAKE) -j$(RELEASETEST_JOBS) -O$(RELEASETEST_OUTPUT) \
	    --no-print-directory _releasetest_phase1; \
	rc=$$?; \
	if [ $$rc -ne 0 ]; then \
	    end_ts=$$(date +%s); \
	    echo "=== releasetest: FAILED in Phase 1 after $$((end_ts - start_ts)) s ==="; \
	    exit $$rc; \
	fi; \
	phase1_ts=$$(date +%s); \
	echo "=== releasetest: Phase 1 passed ($$((phase1_ts - start_ts)) s) — entering Phase 2 ==="; \
	$(MAKE) --no-print-directory _releasetest_phase2; \
	rc=$$?; \
	end_ts=$$(date +%s); \
	if [ $$rc -eq 0 ]; then \
	    echo "=== releasetest: all gates passed ($$((end_ts - start_ts)) s wall-clock," \
	         "Phase 1 $$((phase1_ts - start_ts)) s + Phase 2 $$((end_ts - phase1_ts)) s) ==="; \
	else \
	    echo "=== releasetest: FAILED in Phase 2 after $$((end_ts - start_ts)) s ==="; \
	    exit $$rc; \
	fi

# Internal aggregators for the two phases.  Not for direct use; invoke
# `releasetest` instead.
_releasetest_phase1: $(RELEASETEST_PHASE1)
_releasetest_phase2: $(RELEASETEST_PHASE2)

# libFuzzer — clang-specific (uses libclang_rt.fuzzer, ships with clang's
# compiler-rt).  Builds each harness as a standalone binary against the
# full src/ tree; no .a dependency because libFuzzer needs the sanitizer
# runtimes linked in.  Local-only (no CI); see docs/internals/test-harness.md
# for time-budget guidance.
# --- Stress tests -------------------------------------------------------
#
# test-stress builds and runs 4 GC stress programs against the default
# (URBI_GC_INCREMENTAL) library.  Each program self-asserts and exits 0
# on success.  NOT wired into `make test` (slower path); invoked by
# `make test-stress` or `make releasetest`.

STRESS_BUILDDIR := $(BUILDDIR)/tests/stress

$(STRESS_BUILDDIR):
	@mkdir -p $@

# Stress tests are hosted programs; clock_gettime needs _POSIX_C_SOURCE.
STRESS_CPPFLAGS := $(CPPFLAGS) -D_POSIX_C_SOURCE=200809L

$(STRESS_BUILDDIR)/gc_long_running: tests/stress/gc_long_running.c $(LIB) | $(STRESS_BUILDDIR)
	$(CC) $(CFLAGS) $(STRESS_CPPFLAGS) $< -L$(BUILDDIR) -lurbi -o $@

$(STRESS_BUILDDIR)/gc_many_cycles: tests/stress/gc_many_cycles.c $(LIB) | $(STRESS_BUILDDIR)
	$(CC) $(CFLAGS) $(STRESS_CPPFLAGS) $< -L$(BUILDDIR) -lurbi -o $@

$(STRESS_BUILDDIR)/gc_pause_time: tests/stress/gc_pause_time.c $(LIB) | $(STRESS_BUILDDIR)
	$(CC) $(CFLAGS) $(STRESS_CPPFLAGS) $< -L$(BUILDDIR) -lurbi -o $@

$(STRESS_BUILDDIR)/gc_barrier_throughput: tests/stress/gc_barrier_throughput.c $(LIB) | $(STRESS_BUILDDIR)
	$(CC) $(CFLAGS) $(STRESS_CPPFLAGS) $< -L$(BUILDDIR) -lurbi -o $@

$(STRESS_BUILDDIR)/stress_event_emit_loop: tests/stress/stress_event_emit_loop.c $(LIB) | $(STRESS_BUILDDIR)
	$(CC) $(CFLAGS) $(STRESS_CPPFLAGS) -Isrc $< -L$(BUILDDIR) -lurbi -o $@

test-stress: $(STRESS_BUILDDIR)/gc_long_running \
             $(STRESS_BUILDDIR)/gc_many_cycles \
             $(STRESS_BUILDDIR)/gc_pause_time \
             $(STRESS_BUILDDIR)/gc_barrier_throughput \
             $(STRESS_BUILDDIR)/stress_event_emit_loop
	$(STRESS_BUILDDIR)/gc_long_running
	$(STRESS_BUILDDIR)/gc_many_cycles
	$(STRESS_BUILDDIR)/gc_pause_time
	$(STRESS_BUILDDIR)/gc_barrier_throughput
	$(STRESS_BUILDDIR)/stress_event_emit_loop

# --- GC pause-time regression gate (<1 ms per slice) --------------------
#
# test-gc-pause recompiles gc_pause_time.c with -DGC_PAUSE_ASSERT_NS=1000000
# so that the binary self-asserts max slice < 1 ms and exits non-zero on
# violation.  The standard test-stress target builds WITHOUT that flag so
# the baseline stress run is always threshold-free.
#
# The gated binary lands as gc_pause_time_gated to avoid a stale-rule
# conflict with the unasserted $(STRESS_BUILDDIR)/gc_pause_time above.

$(STRESS_BUILDDIR)/gc_pause_time_gated: tests/stress/gc_pause_time.c $(LIB) | $(STRESS_BUILDDIR)
	$(CC) $(CFLAGS) $(STRESS_CPPFLAGS) -DGC_PAUSE_ASSERT_NS=1000000 \
	    $< -L$(BUILDDIR) -lurbi -o $@

test-gc-pause: $(STRESS_BUILDDIR)/gc_pause_time_gated
	$(STRESS_BUILDDIR)/gc_pause_time_gated
	@echo "test-gc-pause: max slice < 1 ms PASS"

# --- Cross-strategy compile smoke (URBI_GC_NONE) ------------------------
#
# test-gc-none-build verifies that ugc_none.h (the M3 no-op stub) compiles
# cleanly when URBI_GC=URBI_GC_NONE (==2) is set.  Compilation only; no
# link against liburbi.a (real URBI_GC_NONE impl deferred to v2 per
# REVIVAL §2.2 / Row 10 §2.1).
#
# Uses -fsyntax-only (parse + type-check; no object output) so no separate
# build directory is needed.  Both build-smoke files are checked.

test-gc-none-build:
	$(CC) $(CFLAGS) $(CPPFLAGS) -DURBI_GC=2 -fsyntax-only \
	    tests/build/test_gc_none_compile.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -DURBI_GC=2 -fsyntax-only \
	    tests/build/test_gc_none_no_barrier.c
	@echo "test-gc-none-build: URBI_GC_NONE header smoke PASS"

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
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os -mcpu=cortex-m7 -mthumb -ffreestanding \
		        -DURBI_CLEANUP_MAX=16 \
		        -DURBI_STRAND_BUDGET_MAX=200 \
		        -DURBI_GC_SLICE_BUDGET=2048 \
		        -DURBI_WATCHER_POOL_SIZE=16 \
		        -DURBI_WATCHER_READSET_MAX=4 \
		        -DURBI_EVENT_RING_DEPTH=32 \
		        -DURBI_FLOAT_TYPE=4" \
		AR=arm-none-eabi-ar \
		all

cross-riscv:
	$(MAKE) TARGET=riscv-rv32imc \
		CC=riscv64-unknown-elf-gcc \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os -march=rv32imc -mabi=ilp32 -ffreestanding \
		        -DURBI_FLOAT_TYPE=4 \
		        -DURBI_WATCHER_POOL_SIZE=64" \
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
# Scoped to src/*.c only — host-side code under tools/*.c (REPL binary) is not
# subject to the no-globals invariant; signal handlers cannot accept userdata
# pointers, and process-lifetime REPL state has no UVM scope.
tidy: compile_commands.json
	run-clang-tidy -p . -j $$(nproc) -warnings-as-errors='*' -quiet $(SRC)

# Local convenience: run clang-tidy with --fix.  Not invoked by CI.
tidy-fix: compile_commands.json
	run-clang-tidy -p . -j $$(nproc) -fix -format -style=file -quiet $(SRC)

# Strict clang-tidy checklist: bug-prone + cert + analyzer + narrowing.
# Configured via .clang-tidy.strict (parallel to .clang-tidy used by `tidy`).
# Suppressions catalog at .clang-tidy.suppressions.
# Informational at v0.5.7 baseline; promoted to releasetest gate in T118.
.PHONY: test-tidy-strict
test-tidy-strict: ## Run clang-tidy strict checklist over src/
	@bash tools/scripts/run_strict_tidy.sh build/strict-tidy-out.txt

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

# Strict cppcheck — --enable=all --inconclusive over src/ via wrapper script.
# Parallel to the existing `cppcheck` target above (which gates `make lint`
# and uses a narrower checklist). The strict gate uses .cppcheck.suppressions
# for audit-ID-blessed exceptions; closes lock in T118 (releasetest gate).
.PHONY: test-cppcheck
test-cppcheck: ## Run cppcheck --enable=all --inconclusive
	@bash tools/scripts/run_cppcheck.sh build/cppcheck-out.txt

# Static analysis — clang scan-build over the default `make` build.
# Closes a Wave-0 deferral (audit ran scan-build but did not wire the
# target). Emits HTML report under build/scan-build-html/ and a tee'd
# log at build/scan-build-out.txt. Gate promotion to releasetest in T118.
.PHONY: test-scan-build
test-scan-build: ## Run clang scan-build static analyzer
	@bash tools/scripts/run_scan_build.sh build/scan-build-out.txt build/scan-build-html

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
	      --merge-mode-functions=merge-use-line-min \
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

# Branch coverage — same instrumentation as `coverage`, with branch + decision
# tracking. Closes COV-009 audit finding. Uses gcovr's native --branches flag
# rather than lcov (cited by audit) — gcovr is already in PATH and the
# existing coverage target uses it; lcov would add a dep without functional
# benefit.
#
# Threshold gating: informational-only at v0.5.7 baseline (69.4%). Phase 20
# (T119-T125) closes coverage gaps; the gate enables in T118 / T126 once
# the baseline is above 75%. Currently the target reports + writes the
# HTML but does not fail-under.
test-branch-coverage: coverage-tools
	rm -f build/host-coverage/src/*.gcda build/host-coverage/tests/unit/*.gcda
	$(MAKE) TARGET=host-coverage \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O0 -g --coverage" \
		test
	gcovr --root . \
	      --object-directory build/host-coverage \
	      --filter 'src/' \
	      --merge-mode-functions=merge-use-line-min \
	      --branches \
	      --decisions \
	      --txt \
	      --html-details build/host-coverage/branch-report.html
	@echo ""
	@echo "Branch + decision coverage report: build/host-coverage/branch-report.html"
	@echo "(gate enables in v0.5.7-fixes Phase 20 once baseline exceeds 75%)"

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

.PHONY: all test test-asan test-ubsan test-debug test-switch test-determinism test-determinism-default test-determinism-footprint test-determinism-linux cross-arm cross-riscv clean compile_commands.json tidy tidy-fix test-tidy-strict cppcheck test-cppcheck test-scan-build analyzer lint docs-check docs-check-tools coverage coverage-tools test-branch-coverage test-valgrind test-valgrind-deep valgrind-tools fuzz-lex fuzz-parse fuzz-vm fuzz-build fuzz-tools urbi-bin test-integration test-chk releasetest _releasetest_phase1 _releasetest_phase2 test-stress test-gc-none-build test-gc-pause test-loc-cap
