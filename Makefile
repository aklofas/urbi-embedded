# Parked sources: in the tree, never in the build, waiting for Phase 5 to
# re-attach them.  The four files of the cooperative eval service are in
# REPL_CORE_SRCS below; every other src/repl file is here.  The listener,
# auth and queue exist only to coordinate with threads that no longer run;
# urepl_state.c was a vm->repl back-pointer whose only reader was the
# job-queue drain hook in urbi_step; urepl_introspect.c's one live
# primitive now lives in src/stdlib/debug_namespace.c so the dependency
# runs repl -> stdlib; ujson.c was a general JSON reader for the Debug
# namespace, which now hands its answer over as a String.  src/ros/ and
# src/urobotics/ are parked the same way, behind the $(error)s below.
#
# The trace, perf-counter and memory-debug tooling is NOT parked: it was
# part of the old runtime and went with it.  Phase 5 re-derives whatever
# of it the new core wants from git history rather than from a stale
# translation unit that no longer compiles.
REPL_PARKED_SRCS := \
    src/repl/urepl_listener.c \
    src/repl/urepl_auth.c \
    src/repl/urepl_queue.c \
    src/repl/urepl_state.c \
    src/repl/urepl_introspect.c \
    src/repl/ujson.c \
    $(wildcard src/repl/urepl_transport_*.c)

# URBI_BYTECODE_ONLY=1 promotes the v0.6.1 smoke approximation to a real
# pure-strip build: src/lex/, src/parse/, src/emit/ are removed from the
# source list.  Source-taking public entry points (urbi_compile_source,
# urbi_repl_eval) become compile-errors at the call site via header
# gating in <urbi/urbi.h>.  Bytecode-only entry points (urbi_run_chunk,
# urbi_run_script, urbi_load_module) stay unconditional.  T15 + T16 in
# M7 Wave 1.  T17 will follow up to clean any libc-leak unresolved
# symbols surfaced by the real strip.
ifeq ($(URBI_BYTECODE_ONLY),1)
  CPPFLAGS += -DURBI_BYTECODE_ONLY=1
  COMPILER_FRONTEND_DIRS_EXCLUDED := 1
endif

# The cooperative NDJSON eval service is UNCONDITIONAL, not opt-in.  The
# spec keeps "only the eval path, the NDJSON framing it needs, and the
# cooperative step" in the build during the re-foundation, and a service
# that never starts a thread or opens a socket has nothing an embedded
# target needs protecting from.  The networked SERVER is what
# URBI_ENABLE_REPL used to gate, and it is parked with its transports.
REPL_CORE_SRCS := \
       src/repl/urepl.c \
       src/repl/urepl_dispatch.c \
       src/repl/urepl_ndjson.c \
       src/repl/urepl_buffer_transport.c
ifeq ($(URBI_ENABLE_REPL),1)
  $(error URBI_ENABLE_REPL=1 gates the networked REPL server, which is parked during the refound/core re-foundation; the cooperative eval service is in every build)
endif
ifeq ($(URBI_BYTECODE_ONLY),1)
  # No compiler, no eval service.  urbi/repl.h says so with an #error, and
  # the source list has to agree or the link fails on ufront_compile.
  REPL_CORE_SRCS :=
endif

# v0.12.0: opt-in ROS2 bridge component (URBI_ENABLE_ROS2=1).
# Self-contained optional component; requires a hosted build (this tag is
# host-only — the real-DDS / embedded path lands in v0.12.1).
ifeq ($(URBI_ENABLE_ROS2),1)
  ifeq ($(URBI_BYTECODE_ONLY),1)
    $(error URBI_ENABLE_ROS2=1 is incompatible with URBI_BYTECODE_ONLY=1)
  endif
  CPPFLAGS += -DURBI_ENABLE_ROS2=1
  ROS2_SRCS := $(wildcard src/ros/*.c)
  ROS2_GEN_DIR := src/ros/generated
  ROS2_GEN_C   := $(ROS2_GEN_DIR)/ros_msgs.gen.c
  ROS2_GEN_H   := $(ROS2_GEN_DIR)/ros_msgs.gen.h
  ROS2_SRCS    += $(ROS2_GEN_C)

  # v0.12.1: real rcl/rclc/Fast-DDS backend (container-only).  Selected with
  # URBI_ROS_BACKEND=rcl on top of URBI_ENABLE_ROS2=1; adds the validated
  # ROS2 Jazzy include/link flags.  src/ros/uros_rcl.c is already in ROS2_SRCS
  # via the wildcard and is an empty TU unless URBI_ROS_BACKEND_RCL is defined.
  # Include/define flags go in CPPFLAGS (not CFLAGS) so an embedder's
  # command-line CFLAGS= override does not drop the rcl include path.
  ifeq ($(URBI_ROS_BACKEND),rcl)
    ROS2_MSG_PKGS := std_msgs geometry_msgs sensor_msgs builtin_interfaces example_interfaces
    CPPFLAGS += -DURBI_ROS_BACKEND_RCL=1 -I/opt/ros/jazzy/include
    CPPFLAGS += $(foreach d,$(wildcard /opt/ros/jazzy/include/*/),-I$(d))
    LDFLAGS  += -L/opt/ros/jazzy/lib -Wl,-rpath,/opt/ros/jazzy/lib \
                -lrcl -lrclc -lrcutils -lrmw -lrmw_implementation \
                -lrosidl_runtime_c -lrosidl_typesupport_c
    LDFLAGS  += $(foreach p,$(ROS2_MSG_PKGS),-l$(p)__rosidl_typesupport_c -l$(p)__rosidl_generator_c)
    # The rosidl-targeting codegen output: NOT tracked (needs rosidl headers,
    # only generated in-container).  Compiled in addition to the mock gen.
    ROS2_RCL_GEN_C := $(ROS2_GEN_DIR)/ros_msgs_rcl.gen.c
    ROS2_RCL_GEN_H := $(ROS2_GEN_DIR)/ros_msgs_rcl.gen.h
    ROS2_SRCS      += $(ROS2_RCL_GEN_C)
  endif
else
  ROS2_SRCS :=
endif

# v0.12.2: opt-in Standard Robotics API facet overlay (URBI_ENABLE_UROBOTICS=1).
# Pure-urbiscript facets baked into a SEPARATE bytecode blob; off by default
# => zero bytes in the base build, base stdlib blob byte-identical.  Host-only
# this tag (mirrors the ROS2 component); no cross/flavor handling.
ifeq ($(URBI_ENABLE_UROBOTICS),1)
  ifeq ($(URBI_BYTECODE_ONLY),1)
    $(error URBI_ENABLE_UROBOTICS=1 is incompatible with URBI_BYTECODE_ONLY=1)
  endif
  CPPFLAGS += -DURBI_ENABLE_UROBOTICS=1
  UROBOTICS_SRCS := $(wildcard src/urobotics/*.c)
else
  UROBOTICS_SRCS :=
endif

# liburbi.a is exactly four source groups, and every src/ directory that
# is not one of them is either the compiler frontend feeding one or an
# optional component parked for Phase 5 (src/ros, src/urobotics, and the
# networked half of src/repl):
#
#   FRONTEND_SRCS — the compiler frontend (lexer, parser, emitter) and the
#                   bytecode container it produces (src/chunk: writer,
#                   loader, verifier, opcode shapes), plus src/util —
#                   the AST arena, the varint codec, the freestanding
#                   string/zero helpers, and URBI_REQUIRE's failure path.
#                   There is no separate intern TU: the intern seam is
#                   implemented in src/emit/ufront.c over the core's USym
#                   table, so one string table serves compile and run.
#   src/host/     — public API whose implementation is inherently hosted
#                   (the value formatter needs snprintf), kept out of
#                   src/rt/ so the freestanding rule there stays true.
#   RT_SRCS       — the runtime core under src/rt/ plus the standard
#                   library that boots on top of it.
#   REPL_CORE_SRCS — the cooperative NDJSON eval service (four files;
#                   the networked server is parked, see above).
#
# The standard library reaches the core through exactly one header
# (src/rt/ustdlib_glue.h).  Each file exports one or more UMethodDef
# tables; src/rt/uboot.c's table points at them and uboot_init installs
# them.  `every`, `sleep`, Job, Tag and the detach primitives are NOT
# here — they are scheduler state and live in src/rt/usched_natives.c.
# Channel is a script overlay in stdlib.u.  The stdlib blob object is
# listed separately from STDLIB_SRCS so the bake tool can link the
# zero-length stub in its place and avoid a build cycle.
STDLIB_SRCS := \
       src/stdlib/object_root.c \
       src/stdlib/isa_method.c \
       src/stdlib/atoms.c \
       src/stdlib/containers.c \
       src/stdlib/runtime_types.c \
       src/stdlib/namespaces.c \
       src/stdlib/primitives.c \
       src/stdlib/regexp.c \
       src/stdlib/lobby_native.c \
       src/stdlib/debug_namespace.c
STDLIB_BLOB_SRC := src/stdlib/urbi_stdlib_bytecode.gen.c

FRONTEND_SRCS := \
       $(if $(COMPILER_FRONTEND_DIRS_EXCLUDED),,$(wildcard src/lex/*.c)) \
       $(if $(COMPILER_FRONTEND_DIRS_EXCLUDED),,$(wildcard src/parse/*.c)) \
       $(if $(COMPILER_FRONTEND_DIRS_EXCLUDED),,$(wildcard src/emit/*.c)) \
       $(wildcard src/chunk/*.c) \
       $(wildcard src/util/*.c) \
       $(wildcard src/host/*.c)

SRC := $(FRONTEND_SRCS) $(wildcard src/rt/*.c) $(STDLIB_SRCS) $(STDLIB_BLOB_SRC) $(REPL_CORE_SRCS)
TEST_SRC :=

TARGET ?= host
BUILDDIR := build/$(TARGET)

# refactor-3 BLD-03: the optional-component flags must never be combined with
# the bare default build tree.  build/host/ is shared by the bake tool, the
# lint compile database, and every "default build" gate; compiling flag-on
# objects into it leaves stale-flag objects behind (the v0.12.0-H trap —
# previously comment-only convention, now enforced).
ifeq ($(TARGET),host)
  ifeq ($(URBI_ENABLE_ROS2),1)
    $(error URBI_ENABLE_ROS2=1 is parked during the refound/core re-foundation and not buildable on this branch)
  endif
  ifeq ($(URBI_ENABLE_UROBOTICS),1)
    $(error URBI_ENABLE_UROBOTICS=1 is parked during the refound/core re-foundation and not buildable on this branch)
  endif
endif

# The stdlib bytecode blob and its bake tool are parked with src/stdlib/
# until the boot table re-attaches them to the new core.

OBJ := $(patsubst src/%.c,$(BUILDDIR)/src/%.o,$(SRC))
LIB := $(BUILDDIR)/liburbi.a

# The runtime core's own test runner.  It links $(LIB) — the shipped
# archive — rather than a core-only sub-archive: uexec.c calls
# uchunk_destroy and ufront_compile anyway, so a core-only archive was
# never self-contained, and testing the same bytes the embedder links is
# worth more than the separation was.
RT_SRCS   := $(wildcard src/rt/*.c) $(STDLIB_SRCS) $(STDLIB_BLOB_SRC)
RT_TEST_SRCS := $(wildcard tests/rt/test_*.c) tests/rt/runner.c

CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -Os
# v1.0 (B6a) / refactor-3 BLD-05: hide internal cross-TU symbols from the
# export surface.  Lives in a dedicated always-applied variable — NOT a
# `CFLAGS +=` — because every recursive `make CFLAGS="..."` invocation
# (sanitizers, -O variants, trace/perf/memdbg presets, cross builds)
# overrides CFLAGS from the command line, which silently dropped the append:
# only the default host build was actually built hidden.  Public API is
# re-exported via `#pragma GCC visibility push(default)` in the
# include/urbi/*.h headers.
URBI_VIS_FLAGS := -fvisibility=hidden
CPPFLAGS += -Iinclude -Isrc

# refactor-3 BLD-04: flag stamp.  Any change to the compiler, CFLAGS, or
# CPPFLAGS invalidates every object in this BUILDDIR — the root cause of the
# whole stale-object trap family (v0.12.0-H et al.) and of CI's defensive
# `make clean`s.  Compare-and-swap recipe: the stamp file is rewritten ONLY
# when the flag string actually changed, so an unchanged flag set never
# triggers rebuilds.  FLAGS_CONTENT is recursively expanded (=) so it picks
# up the final values at recipe time.  (The stamp RULES live below, after
# `all:`, so a recipe-less rule here cannot steal the default goal.)
FLAGSTAMP := $(BUILDDIR)/.flags
FLAGS_CONTENT = $(CC) | $(CFLAGS) | $(URBI_VIS_FLAGS) | $(CPPFLAGS)
RUNNER_WRAPPER ?=

# v0.10.10-E followup: auto-include dependency files generated by -MMD -MP.
# Each .d file lists header dependencies for one .o file (+ phony entries
# per header so a deleted header doesn't break the build).  Use sinclude
# so initial build (no .d files yet) doesn't warn.
# The .d files precede `all:`, so without an explicit default goal a bare
# `make` in an already-built tree picks up the first .d rule instead.
.DEFAULT_GOAL := all
# BLD-CI-4: scope the dep-file glob to THIS build's $(BUILDDIR), not the whole
# build/ tree.  Pulling in every target's .d files (build/arm-cortex-m7/,
# build/host-asan/, ...) is wasted work for a host `make` and risks a stale .d
# from another TARGET shadowing a header rule in this one.
sinclude $(shell find $(BUILDDIR) -name '*.d' 2>/dev/null)

all: $(LIB) $(BUILDDIR)/urbi

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

$(BUILDDIR)/tests/rt/runner: $(RT_TEST_SRCS) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -Isrc -Itests/rt -o $@ $(RT_TEST_SRCS) $(LIB) -lm

.PHONY: test-rt check-rt-layering
test-rt: $(BUILDDIR)/tests/rt/runner check-rt-layering
	$(RUNNER_WRAPPER) $<
check-rt-layering:
	sh tests/scripts/check_rt_layering.sh

# The frontend's own runner: lexer, parser, arena, emitter, chunk
# writer/loader/verifier/disassembler, varint, intern, and the public
# header's inline value constructors.  Nothing here starts a strand —
# the runtime has tests/rt/ and the .chk corpus.  It reaches internal
# frontend headers (-Isrc), which is why it links the archive rather
# than being an embedder-facing example.
UNIT_TEST_SRCS := $(wildcard tests/unit/test_*.c) tests/unit/runner.c

$(BUILDDIR)/tests/unit/runner: $(UNIT_TEST_SRCS) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -Isrc -Itests/unit -o $@ $(UNIT_TEST_SRCS) $(LIB) -lm

test-unit: $(BUILDDIR)/tests/unit/runner
	$(RUNNER_WRAPPER) $<

# --- footprint and performance probes -----------------------------------
#
# Four numbers the project has committed to: what a booted VM costs, what
# an idle strand costs, whether a ten-thousand-iteration loop gives its
# memory back, and how the core compares to the one it replaced.  Each
# probe prints what it measured and exits non-zero when it missed, so the
# numbers in the release notes and the docs can be regenerated rather than
# trusted.  See tests/probes/probe.h.
#
# The three MEMORY probes are in `make test`: their numbers are
# deterministic and a busy machine does not change them.
#
# The TIMING probe is not, and cannot be.  `make test` is itself one gate
# of a 20-way parallel releasetest sweep, and a wall-clock measurement
# taken while nineteen other compiles saturate the box measures the box
# (observed: 4.99x under -j32 against 1.46x solo).  It gets its own
# target, `test-bench`, which no aggregate runs: its baseline is wall-clock
# seconds recorded on one machine, so on any other machine (a CI runner)
# the ratio compares two machines.  It takes the binary and
# the fixture directory as arguments and is never $(RUNNER_WRAPPER)'d:
# timing an instrumented binary against an uninstrumented baseline would
# compare nothing.
PROBE_SRCS := $(wildcard tests/probes/*.c)
PROBE_BINS := $(patsubst tests/probes/%.c,$(BUILDDIR)/tests/probes/%,$(PROBE_SRCS))

$(BUILDDIR)/tests/probes/%: tests/probes/%.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -Isrc -Itests/probes -o $@ $< $(LIB) -lm

.PHONY: test-probes test-bench
test-probes: $(PROBE_BINS) $(BUILDDIR)/urbi
	@$(RUNNER_WRAPPER) $(BUILDDIR)/tests/probes/boot_heap
	@$(RUNNER_WRAPPER) $(BUILDDIR)/tests/probes/strand_cost
	@$(RUNNER_WRAPPER) $(BUILDDIR)/tests/probes/leaks

# Run this alone.  Under `make -j` beside anything else the number is the
# machine's, not the interpreter's.  Run it on the machine that recorded
# tests/probes/baseline-timings.md; it is not part of releasetest.
test-bench: $(PROBE_BINS) $(BUILDDIR)/urbi
ifeq ($(TARGET),host)
	@$(BUILDDIR)/tests/probes/lookup_bench $(BUILDDIR)/urbi tests/probes
else
	@echo "lookup_bench: SKIP — $(TARGET) is instrumented or built at a"
	@echo "  different optimization level, and the baseline it compares"
	@echo "  against was recorded on the default host build."
endif

# Core archive. Kept as its own target for cross-compile / freestanding
# consumers that build the library without the host tools.
core: $(LIB)

# refactor-3 BLD-04: flag-stamp rules (variables defined above, before the
# first prerequisite-list use).
.PHONY: force-flagstamp
force-flagstamp:
$(FLAGSTAMP): force-flagstamp
	@mkdir -p $(@D)
	@printf '%s\n' "$(FLAGS_CONTENT)" | cmp -s - $@ 2>/dev/null || \
	    printf '%s\n' "$(FLAGS_CONTENT)" > $@

$(BUILDDIR)/src/%.o: src/%.c $(FLAGSTAMP)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(URBI_VIS_FLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

# v0.12.0: ROS2 message marshaling codegen.  Tracked, regenerated from the
# manifest by tools/urbi-rosgen.py.  Order-only prereq guarantees the
# generated header exists before any src/ros/*.o (incl. the generated .c)
# compiles.
ifeq ($(URBI_ENABLE_ROS2),1)
# refactor-3 BLD-01: grouped target (&:) — one codegen run produces both
# files; a plain two-target rule would run the recipe once PER target
# under -j and race against itself.
$(ROS2_GEN_C) $(ROS2_GEN_H) &: tools/urbi-rosgen.py src/ros/msgs/manifest.json
	@mkdir -p $(ROS2_GEN_DIR)
	python3 tools/urbi-rosgen.py src/ros/msgs/manifest.json $(ROS2_GEN_C) $(ROS2_GEN_H)
# refactor-3 BLD-01: explicit per-object prerequisites.  (A recipe-less
# PATTERN rule here would CANCEL the %.o pattern, not add a prereq —
# that was the original bug.)
ROS2_OBJS := $(patsubst src/%.c,$(BUILDDIR)/src/%.o,$(ROS2_SRCS))
$(ROS2_OBJS): $(ROS2_GEN_H)
endif

ifeq ($(URBI_ROS_BACKEND),rcl)
$(ROS2_RCL_GEN_C) $(ROS2_RCL_GEN_H) &: tools/urbi-rosgen.py src/ros/msgs/manifest.json
	@mkdir -p $(ROS2_GEN_DIR)
	python3 tools/urbi-rosgen.py --target rcl src/ros/msgs/manifest.json $(ROS2_RCL_GEN_C) $(ROS2_RCL_GEN_H)
$(ROS2_OBJS): $(ROS2_RCL_GEN_H)
endif

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
$(BUILDDIR)/tools/linenoise.o: tools/linenoise.c $(FLAGSTAMP) | $(BUILDDIR)/tools
	$(CC) -std=c99 -Os -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=700 \
	    -w -Itools -MMD -MP -c -o $@ $<

$(BUILDDIR)/tools/urbi.o: tools/urbi.c $(FLAGSTAMP) | $(BUILDDIR)/tools
	$(CC) $(CFLAGS) $(URBI_VIS_FLAGS) $(CPPFLAGS) -Itools -MMD -MP -c -o $@ $<

$(BUILDDIR)/urbi: $(BUILDDIR)/tools/urbi.o $(BUILDDIR)/tools/linenoise.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $(BUILDDIR)/tools/urbi.o $(BUILDDIR)/tools/linenoise.o $(LIB) -lm

urbi-bin: $(BUILDDIR)/urbi

# --- .chk host driver ---------------------------------------------------
#
# The fixtures carrying `## host:` directives need more than one realm, a
# clock the fixture controls, and urbi_step called where the fixture says.
# run_chk.sh looks for this binary beside the urbi binary, so every
# sanitizer variant picks up its own instrumented copy.

$(BUILDDIR)/chk-host-driver: tests/integration/chk_host_driver.c $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LIB) -lm

.PHONY: chk-host-driver
chk-host-driver: $(BUILDDIR)/chk-host-driver

# --- REPL .chk driver ---------------------------------------------------
#
# tests/chk/repl/*.chk are NDJSON, not urbiscript, and their expectation
# lines are substring sets rather than exact output, so the match happens
# inside the driver and the verdict is its exit status.  It reaches one
# internal header (the in-process buffer transport) because a stream with
# no socket behind it is exactly what a deterministic fixture needs.
$(BUILDDIR)/repl-chk-driver: tests/integration/repl_chk_driver.c $(LIB)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CPPFLAGS) -Iinclude -Isrc -o $@ $< $(LIB) -lm

.PHONY: repl-chk-driver
repl-chk-driver: $(BUILDDIR)/repl-chk-driver

# --- Stdlib bake tool (host-only) ---------------------------------------
#
# tools/urbi-compile-stdlib compiles src/stdlib/stdlib.u into
# src/stdlib/urbi_stdlib_bytecode.gen.c, the blob uboot_init loads.
#
# CYCLE BREAK.  The tool needs the compiler, which lives in liburbi.a,
# which contains the .gen.o built from the file the tool produces.  So it
# links the library objects DIRECTLY, minus .gen.o, plus
# tools/stub_stdlib_bytecode.c -- the same two symbols with a zero
# length, which uboot_init treats as "no script overlay".
#
# ALWAYS build/host.  The bake runs once with the native compiler and its
# output ships as portable C source, so a cross or sanitizer sub-make
# neither needs the tool nor should rebuild it with its own flags (that
# would clobber the host binary and churn the tracked .gen.c).
#
# .gen.c is TRACKED, not a build artifact: the first build of liburbi.a
# must not require a tool that requires liburbi.a.
BAKE_SRCS := $(filter-out $(STDLIB_BLOB_SRC),$(FRONTEND_SRCS) $(wildcard src/rt/*.c) $(STDLIB_SRCS))
BAKE_OBJS := $(patsubst src/%.c,build/host/src/%.o,$(BAKE_SRCS))

# Host-object pattern for cross and sanitizer sub-makes, which have their
# own $(BUILDDIR).  Guarded so it does not duplicate the standard pattern
# rule when $(BUILDDIR) already IS build/host.
ifneq ($(TARGET),host)
build/host/src/%.o: src/%.c
	@mkdir -p $(@D)
	cc -std=c99 -Wall -Wextra -Wpedantic -Os -fvisibility=hidden -Iinclude -Isrc -MMD -MP -c -o $@ $<
endif

build/host/tools/stub_stdlib_bytecode.o: tools/stub_stdlib_bytecode.c
	@mkdir -p $(@D)
	cc -std=c99 -Os -Iinclude -Isrc -MMD -MP -c -o $@ $<

tools/urbi-compile-stdlib: tools/urbi-compile-stdlib.c $(BAKE_OBJS) \
                           build/host/tools/stub_stdlib_bytecode.o
	cc -std=c99 -Wall -Wextra -Wpedantic -Os -Iinclude -Isrc -o $@ $< \
	    $(BAKE_OBJS) build/host/tools/stub_stdlib_bytecode.o -lm

# Re-baking is EXPLICIT, never a build prerequisite.  The blob and the
# tool both live outside $(BUILDDIR), so a rule that fires during an
# ordinary build fires in every sanitizer, coverage and analyzer
# sub-make at once: under -j they race to link the tool and to rewrite a
# tracked source, and one sub-make executes a binary another is still
# writing ("Permission denied").  A stale blob is caught by the freshness
# gate below instead, which is what a tracked generated file is for.
.PHONY: bake-stdlib
bake-stdlib: tools/urbi-compile-stdlib
	./tools/urbi-compile-stdlib src/stdlib/stdlib.u src/stdlib/urbi_stdlib_bytecode.gen.c

# Drift gate: re-bake and diff against the tracked file, so a stdlib.u
# edit that was never baked fails the build rather than shipping stale
# bytecode.  Determinism gate: three bakes of one input must be
# byte-identical, or the wire-format hashes churn on every build.
#
# test-bake-smoke waits on the freshness gate rather than running beside
# it: both need tools/urbi-compile-stdlib, and two parallel sub-makes
# linking one out-of-BUILDDIR binary is the same race as above.
.PHONY: test-stdlib-bytecode-fresh test-bake-smoke
test-stdlib-bytecode-fresh: tools/urbi-compile-stdlib
	@./tests/scripts/check-stdlib-fresh.sh
test-bake-smoke: test-stdlib-bytecode-fresh
	@bash tests/scripts/bake_smoke.sh

# --- Integration tests --------------------------------------------------
#
# test-integration runs the REPL shell harness against the built binary.
# Folded into the existing `test` aggregate so it runs under every
# sanitizer variant automatically. The shell script itself is NOT wrapped
# by $(RUNNER_WRAPPER) because dash's own "still-reachable" blocks break
# valgrind; the urbi binary is memory-clean when invoked directly.

test-integration: $(BUILDDIR)/urbi
	tests/integration/repl_smoke.sh $(BUILDDIR)/urbi

# test-batch-errors is the gate for the OTHER entry point.  Every .chk
# fixture but one runs through `urbi -i` or the host driver, so nothing in
# the corpus watches `urbi -e` / `urbi -f`: exit status on an uncaught
# throw, chunk-top fork separators, and whether a script that parks ever
# resumes.  It was dropped from `make test` during the re-foundation and
# went red unnoticed; it is a direct prerequisite again for that reason.
.PHONY: test-batch-errors
test-batch-errors: $(BUILDDIR)/urbi
	@URBI=$(BUILDDIR)/urbi bash tests/scripts/test-batch-errors.sh

# --- .chk conformance fixtures -----------------------------------------
#
# test-chk runs every tests/chk/**/*.chk fixture against the built urbi
# binary through tests/integration/chk_summary.sh, one REPL session per
# fixture.  The whole corpus is always run and reported per directory, so
# the tally shows how much of the language the current core covers; only
# the directories in CHK_GATE_DIRS decide pass/fail, and they widen as
# each subsystem is re-founded.  tests/chk/bringup-exclusions.txt is the
# ratchet for individual fixtures a later subsystem still blocks.
#
# tests/chk/repl/*.chk are NDJSON fixtures for the eval service rather
# than urbiscript; they carry `## mode: repl` and run through
# repl-chk-driver, which does its own matching.
#
# Not valgrind-wrapped: urbi itself is memory-clean, and wrapping the
# sh+awk+sed pipeline adds noise, not signal.
CHK_GATE_DIRS ?= arithmetic closure function control \
                 objects globals stdlib lobby operators repl \
                 exceptions control_transfer \
                 separator scheduler tag temporal mutex semaphore \
                 chunk_lifecycle reactive lazy migration

test-chk: $(BUILDDIR)/urbi $(BUILDDIR)/chk-host-driver $(BUILDDIR)/repl-chk-driver
	@CHK_GATE_DIRS="$(CHK_GATE_DIRS)" sh tests/integration/chk_summary.sh $(BUILDDIR)/urbi

# The embedding guide's samples are compiled, not asserted: every C block
# in docs/embedding-guide.md is extracted and built against the archive,
# so an API change that the guide does not follow fails the build.
.PHONY: test-embedding-guide
test-embedding-guide: $(LIB)
	@bash tests/integration/test_embedding_guide_compiles.sh $(BUILDDIR)

# refactor-3 CHK meta-gate: pins run_chk.sh's exit-code contract with stub
# binaries (no VM involved).  Must stay green across any future runner edit.
.PHONY: test-chk-runner
test-chk-runner:
	@bash tests/integration/test_run_chk_runner.sh

# `make test`: the frontend runner, the runtime runner, the layering
# gate, the .chk corpus driven through the urbi binary, and the two shell
# harnesses that cover what the corpus cannot see -- the REPL smoke run
# and the batch (-e / -f) entry point.
test: $(LIB) test-unit test-rt check-rt-layering test-chk test-probes \
      test-integration test-batch-errors

.PHONY: test-wire-format-determinism
test-wire-format-determinism: $(BUILDDIR)/urbi
	@./tests/scripts/check_wire_format_determinism.sh

# API manifest gate — verifies that every urbi_ symbol exported from
# liburbi.a is enumerated in docs/api-surface-tiers.md.
# Catches new internal symbols accidentally becoming public and ensures the
# manifest stays in sync with the library.  Closes audit-1 F13 /
# api-ergonomics F12.  See tests/scripts/check-api-manifest.sh.
.PHONY: test-api-manifest
test-api-manifest: $(LIB)
	@./tests/scripts/check-api-manifest.sh $(BUILDDIR)

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
# switch-based VM dispatch path even on GCC/Clang.  Keeps both dispatch
# paths compiling and passing continuously.
test-switch:
	$(MAKE) TARGET=host-switch \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os -DURBI_VM_FORCE_SWITCH=1" \
		test

# test-cache-verify — every slot-cache hit also runs the uncached lookup
# and traps when the two disagree.  The whole aggregate runs under it, so
# a stale entry anywhere in the corpus is a crash with a fixture name on
# it rather than a wrong value nobody compared.
.PHONY: test-cache-verify
test-cache-verify:
	$(MAKE) TARGET=host-cache-verify \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -DURBI_SLOT_CACHE_VERIFY=1" \
		test

# refactor-3 TEST-GAP-03: -O2 build variant.  The matrix was -Os/-O0/-O1
# only; the v0.10.11 channel_proto bug was -Os-specific, proving the suite
# is optimization-level sensitive.  Runs the full unit+integration+chk
# aggregate at the optimization level desktop embedders actually use.
test-o2:
	$(MAKE) TARGET=host-o2 \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O2 -g" \
		test

# test-gc-stress — refactor-3 TEST-GAP-01: full suite under URBI_GC_STRESS=1
# (synchronous full collection before EVERY GC-cell allocation — the
# highest-leverage detector for the rooting-gap bug class: catch_value
# v0.11.4, walk_uevent v1.0 hang, container elements B2).  -O1 keeps
# wall-clock tolerable.  Own TARGET= so Phase 1 -j parallelism stays
# race-free.  In RELEASETEST_PHASE1 since v0.13.2 (corpus green; ~2 min
# wall-clock solo).
.PHONY: test-gc-stress
test-gc-stress:
	$(MAKE) TARGET=host-gc-stress \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -DURBI_GC_STRESS=1" \
		test

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
# in parallel with the sanitizer matrix + lint + coverage etc., which
# IS what releasetest does.
# URBI_SKIP_THREAD_FUZZ_TESTS skips event_ring_multi_thread_fuzz_100k, which
# memcheck cannot meaningfully run: it serializes threads onto one CPU, so
# the SPSC producer fills the ring then busy-spins on RING_FULL while the
# consumer is descheduled — the test loses race-detection value AND pushes
# wall-clock past 30 min (v0.8.2 wedge symptom). The test still runs under
# `make test` and the sanitizer variants where threads do execute concurrently.
test-valgrind: valgrind-tools
	$(MAKE) TARGET=host-valgrind \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -DURBI_SKIP_THREAD_FUZZ_TESTS=1" \
		RUNNER_WRAPPER="valgrind --tool=memcheck --error-exitcode=1 --leak-check=full -q" \
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
	CHK_GATE_DIRS="$(CHK_GATE_DIRS)" bash tests/integration/test_full_corpus_sanitize.sh

# --- Release test aggregate --------------------------------------------
#
# releasetest runs every host-side gate the CI matrix runs, in parallel.
# Cross-compile, REPL-server, ROS2, urobotics, trace, perf-counters, and
# mem-debug gates are parked (refound/core) and excluded — see
# REPL_PARKED_SRCS above.
#
# Runtime: ~5 minutes on a 32-core / 64 GB box (dominated by the two
# valgrind passes; sanitizer variants and analysis run alongside them).
# Invoked manually before tagging a release, or before pushing a branch
# that touches multiple subsystems.
#
# All sub-targets use disjoint $(BUILDDIR) trees ($(TARGET)=host /
# host-asan / host-ubsan / host-debug / host-switch / host-valgrind /
# host-coverage / host-analyzer), so concurrent
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
# Phase 19 (v0.5.8-cleanup) drove test-cppcheck strict residuals 135 → 0
# and promoted it to hard-fail (the lint aggregate runs the narrow advisory
# cppcheck target; this is the --enable=all --inconclusive strict variant
# gated via .cppcheck.suppressions).
# Phase 20 (v0.5.8-cleanup) drove test-tidy-strict residuals 23 → 0 across
# bugprone-branch-clone, performance-no-int-to-ptr (UProtos pointer-encoding
# design pin), clang-analyzer-valist.Uninitialized, optin.performance.Padding
# (UVM struct layout pin), bugprone-too-small-loop-variable,
# bugprone-misplaced-widening-cast, bugprone-macro-parentheses; promoted to
# hard-fail.
RELEASETEST_PHASE1 := \
    test test-asan test-ubsan test-debug test-switch \
    test-gc-stress test-cache-verify \
    lint docs-check coverage \
    test-scan-build test-cppcheck test-tidy-strict \
    test-wire-format-determinism \
    test-stdlib-bytecode-fresh test-bake-smoke \
    test-api-manifest \
    test-chk-runner test-fuzz-smoke test-o2 test-embedding-guide
# Phase 2: valgrind, running alone after Phase 1 finishes.
# Empirically valgrind throughput collapses by 10-20× when sharing memory
# bandwidth with concurrent gcov / clang-tidy / cppcheck / fanalyzer
# (instrumented runner balloons from ~2 min solo to 40+ min under
# contention).  Phase 2 is sequential — the cumulative wall-clock with
# Phase 1 first is still substantially faster than the original 15-min
# fully-sequential design.
RELEASETEST_PHASE2 := test-valgrind test-corpus-sanitize

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
FUZZ_BUILDDIR := build/host-fuzz
FUZZ_CC       ?= clang
FUZZ_CFLAGS   := -std=c99 -Wall -Wextra -Wpedantic -O1 -g \
                 -fsanitize=fuzzer,address,undefined \
                 -fno-omit-frame-pointer

$(FUZZ_BUILDDIR):
	@mkdir -p $@

# refactor-3 TEST-GAP-02 fix: $(SRC) filters out the stdlib bytecode .gen.c
# (the OBJ list adds its object separately), so passing bare $(SRC) here had
# bit-rotted the fuzz link ("undefined reference to urbi_stdlib_bytecode_len").
FUZZ_SRC := $(SRC)

$(FUZZ_BUILDDIR)/fuzz_lex: tests/fuzz/fuzz_lex.c $(FUZZ_SRC) | $(FUZZ_BUILDDIR)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(CPPFLAGS) -o $@ $(FUZZ_SRC) tests/fuzz/fuzz_lex.c -lm

$(FUZZ_BUILDDIR)/fuzz_parse: tests/fuzz/fuzz_parse.c $(FUZZ_SRC) | $(FUZZ_BUILDDIR)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(CPPFLAGS) -o $@ $(FUZZ_SRC) tests/fuzz/fuzz_parse.c -lm

$(FUZZ_BUILDDIR)/fuzz_vm: tests/fuzz/fuzz_vm.c $(FUZZ_SRC) | $(FUZZ_BUILDDIR)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(CPPFLAGS) -o $@ $(FUZZ_SRC) tests/fuzz/fuzz_vm.c -lm

# refactor-4 REPL-N1: the bytecode chunk loader — the surface with the actual
# stack-overflow finding (B3).  Links the full runtime like fuzz_vm/lex/parse.
$(FUZZ_BUILDDIR)/fuzz_chunk: tests/fuzz/fuzz_chunk.c $(FUZZ_SRC) | $(FUZZ_BUILDDIR)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(CPPFLAGS) -o $@ $(FUZZ_SRC) tests/fuzz/fuzz_chunk.c -lm

fuzz-lex: fuzz-tools $(FUZZ_BUILDDIR)/fuzz_lex
	@echo "running fuzz_lex (Ctrl-C to stop; use -runs=N for bounded)"
	$(FUZZ_BUILDDIR)/fuzz_lex

fuzz-parse: fuzz-tools $(FUZZ_BUILDDIR)/fuzz_parse
	@echo "running fuzz_parse (Ctrl-C to stop; use -runs=N for bounded)"
	$(FUZZ_BUILDDIR)/fuzz_parse

fuzz-vm: fuzz-tools $(FUZZ_BUILDDIR)/fuzz_vm
	@echo "running fuzz_vm (Ctrl-C to stop; use -runs=N for bounded)"
	$(FUZZ_BUILDDIR)/fuzz_vm

fuzz-chunk: fuzz-tools $(FUZZ_BUILDDIR)/fuzz_chunk
	@echo "running fuzz_chunk (Ctrl-C to stop; use -runs=N for bounded)"
	$(FUZZ_BUILDDIR)/fuzz_chunk tests/fuzz/seeds/chunk/

fuzz-build: fuzz-tools $(FUZZ_BUILDDIR)/fuzz_lex $(FUZZ_BUILDDIR)/fuzz_parse $(FUZZ_BUILDDIR)/fuzz_vm $(FUZZ_BUILDDIR)/fuzz_chunk

# refactor-3 TEST-GAP-02: bounded fuzz smoke for releasetest Phase 1.
# -runs=20000 per harness (sub-second each; -max_total_time bounds pathology).
# Loud SKIP when clang/libFuzzer is unavailable — the CI releasetest job
# installs clang, so the gate is real there.
.PHONY: test-fuzz-smoke
test-fuzz-smoke:
	@if ! command -v $(FUZZ_CC) >/dev/null 2>&1; then \
	    echo "================================================================"; \
	    echo "test-fuzz-smoke: SKIP — $(FUZZ_CC) not found in PATH."; \
	    echo "Install clang + libclang-rt-<ver>-dev to run the fuzz smoke."; \
	    echo "================================================================"; \
	    exit 0; \
	fi
	@$(MAKE) --no-print-directory $(FUZZ_BUILDDIR)/fuzz_lex $(FUZZ_BUILDDIR)/fuzz_parse $(FUZZ_BUILDDIR)/fuzz_vm $(FUZZ_BUILDDIR)/fuzz_chunk
	$(FUZZ_BUILDDIR)/fuzz_lex   -runs=20000 -max_total_time=120
	$(FUZZ_BUILDDIR)/fuzz_parse -runs=20000 -max_total_time=120
	$(FUZZ_BUILDDIR)/fuzz_vm    -runs=20000 -max_total_time=120
	$(FUZZ_BUILDDIR)/fuzz_chunk -runs=20000 -max_total_time=120 tests/fuzz/seeds/chunk/
	@echo "test-fuzz-smoke: 4 harnesses x 20000 bounded runs clean"

fuzz-tools:
	@command -v $(FUZZ_CC) >/dev/null 2>&1 || { \
	    echo "error: $(FUZZ_CC) not found in PATH"; \
	    echo "install: sudo apt-get install -y clang libclang-rt-18-dev"; \
	    exit 1; \
	}

# Compilation database for clangd / CLion / VS Code indexing.
# Generated on demand; gitignored. Re-run after changing CFLAGS/CPPFLAGS or
# adding/removing source files.
compile_commands.json:
	@printf '[\n' > $@
	@first=1; for f in $(SRC) tools/urbi.c tools/linenoise.c; do \
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
	@bash tools/scripts/run_strict_tidy.sh build/strict-tidy-out.txt $(SRC)

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
	@bash tools/scripts/run_cppcheck.sh build/cppcheck-out.txt $(SRC)

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
	find build/host-coverage -name '*.gcda' -delete 2>/dev/null || true
	$(MAKE) TARGET=host-coverage \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O0 -g --coverage" \
		test
	# Line-coverage floor, re-baselined at the core re-foundation.  It went
	# 85 -> 80 when the runtime-internals unit tests were deleted and 80 ->
	# 75 during bring-up, while most of the corpus was waiting on subsystems
	# that had not landed.  All of them have now, the frontend runner is back
	# in the build, and the measured figure is 89%.  The floor sits a few
	# points below that so ordinary churn does not trip it; raise it, never
	# lower it.
	gcovr --root . \
	      --object-directory build/host-coverage \
	      --filter 'src/' \
	      --merge-mode-functions=merge-use-line-min \
	      --fail-under-line 85 \
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

# Aggregate: gating tidy, advisory cppcheck, advisory analyzer.
# CI invokes this as one step per-target so failures clearly name
# which tool caught the issue.
lint: tidy cppcheck analyzer

clean:
	rm -rf build compile_commands.json

# ---- documentation verification ------------------------------------------
#
# docs-check runs markdown lint + intra-repo link checking over docs/, the
# top-level README / CONTRIBUTING / CHANGELOG, example READMEs, component
# READMEs, and tests/qemu docs. Generated build/ and _deps/ trees are
# excluded so vendored pico-sdk / ESP-IDF sources are not scanned.
# Gated in CI via the docs-check job (see .github/workflows/ci.yml).
# Requires markdownlint-cli2 and markdown-link-check in PATH; install with:
#     npm install -g markdownlint-cli2@0.13 markdown-link-check@3.12

DOCS_LINT_TARGETS := 'docs/**/*.md' README.md CONTRIBUTING.md CHANGELOG.md \
    'examples/**/*.md' 'components/**/*.md' 'tests/qemu/**/*.md' \
    '!**/build/**' '!**/_deps/**'

docs-check: docs-check-tools
	markdownlint-cli2 --config .markdownlint.yaml $(DOCS_LINT_TARGETS)
	@echo "--- link-check ---"
	@find docs examples components tests/qemu \
	    README.md CONTRIBUTING.md CHANGELOG.md \
	    -name '*.md' -type f \
	    ! -path '*/build/*' ! -path '*/_deps/*' \
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

# ---- version sync gate -------------------------------------------------------
#
# Checks that the ESP-IDF component manifest version, README.md version
# strings, and include/urbi/version.h all agree on the release being
# prepared.  Run by `make check-version-sync` and by the version-sync GHA
# job; deliberately NOT a dep of docs-check, which must stay runnable
# without a git tag history.

check-version-sync:
	@tests/scripts/check-version-sync.sh

.PHONY: test-unit test-probes test-bench test-embedding-guide
.PHONY: all core test test-asan test-ubsan test-debug test-switch test-cache-verify clean compile_commands.json tidy tidy-fix test-tidy-strict cppcheck test-cppcheck test-scan-build analyzer lint docs-check docs-check-tools check-version-sync coverage coverage-tools test-valgrind valgrind-tools fuzz-lex fuzz-parse fuzz-vm fuzz-chunk fuzz-build fuzz-tools urbi-bin test-integration test-chk releasetest _releasetest_phase1 _releasetest_phase2 test-api-manifest test-gc-stress test-chk-runner test-fuzz-smoke test-o2 force-flagstamp
