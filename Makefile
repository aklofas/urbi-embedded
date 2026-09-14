# Aux layer — separate translation unit, separate archive. Filtered out
# of the core SRC list below so liburbi.a (core) stays free of aux symbols.
# Embedders opt into the aux layer by linking -laux at link time. See
# CONTRIBUTING.md "Aux layer governance" and include/urbi/aux.h.
AUX_SRCS := src/urbi_aux.c

# refound/core: parked sources.  Filtered out of the default source lists
# below so the files stay in the tree (a later v1.x REPL-server /
# trace-tooling re-attachment reads them) but never enter the build.
# The REPL network listener, session auth, and outbound queue only serve
# the networked REPL server (parked); urepl.c/urepl_dispatch.c reference
# their headers but nothing in this build calls into them, so they compile
# cleanly into the archive without ever being pulled into a link.
# src/runtime/utrace_format.c, uperf.c, and umemdebug.c compile to empty
# translation units whenever their feature macro is off (no unconditional
# public-API stub, unlike utrace.c's URBI_TRACE=0 branch) — parking them
# changes nothing about the default archive.
REPL_PARKED_SRCS := \
    src/repl/urepl_listener.c \
    src/repl/urepl_auth.c \
    src/repl/urepl_queue.c \
    $(wildcard src/repl/urepl_transport_*.c)
RUNTIME_PARKED_SRCS := \
    src/runtime/utrace_format.c \
    src/runtime/uperf.c \
    src/runtime/umemdebug.c

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

# v0.9.1 — opt-in REPL service over TCP/Unix/UART.  Requires the
# compiler frontend (URBI_BYTECODE_ONLY=0); the combination is rejected
# at the Makefile level because urbi_repl_eval cannot exist without
# src/lex/, src/parse/, src/emit/ linked in.  Adds src/repl/*.c to the
# core archive.
ifeq ($(URBI_ENABLE_REPL),1)
  ifeq ($(URBI_BYTECODE_ONLY),1)
    $(error URBI_ENABLE_REPL=1 is incompatible with URBI_BYTECODE_ONLY=1)
  endif
  CPPFLAGS += -DURBI_ENABLE_REPL=1
  ifeq ($(URBI_REPL_COOPERATIVE_ONLY),1)
    CPPFLAGS += -DURBI_REPL_COOPERATIVE_ONLY=1
  endif
  # refound/core: the listener, auth, queue, and transports are parked
  # (see REPL_PARKED_SRCS above) regardless of URBI_REPL_COOPERATIVE_ONLY —
  # that flag now only controls the CPPFLAGS define kept-file callers read.
  REPL_SRCS := $(filter-out $(REPL_PARKED_SRCS),$(wildcard src/repl/*.c))
else
  REPL_SRCS :=
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

SRC := $(filter-out $(AUX_SRCS), \
       $(wildcard src/*.c)) \
       $(if $(COMPILER_FRONTEND_DIRS_EXCLUDED),,$(wildcard src/lex/*.c)) \
       $(if $(COMPILER_FRONTEND_DIRS_EXCLUDED),,$(wildcard src/parse/*.c)) \
       $(if $(COMPILER_FRONTEND_DIRS_EXCLUDED),,$(wildcard src/emit/*.c)) \
       $(wildcard src/vm/*.c) \
       $(wildcard src/gc/*.c) \
       $(wildcard src/sched/*.c) \
       $(wildcard src/watcher/*.c) \
       $(wildcard src/event/*.c) \
       $(wildcard src/tag/*.c) \
       $(wildcard src/changed/*.c) \
       $(wildcard src/chunk/*.c) \
       $(wildcard src/value/*.c) \
       $(filter-out $(RUNTIME_PARKED_SRCS),$(wildcard src/runtime/*.c)) \
       $(wildcard src/realm/*.c) \
       $(wildcard src/object/*.c) \
       $(filter-out src/stdlib/urbi_stdlib_bytecode.gen.c,$(wildcard src/stdlib/*.c)) \
       $(REPL_SRCS) \
       $(ROS2_SRCS) \
       $(UROBOTICS_SRCS)
TEST_SRC := $(wildcard tests/unit/test_*.c) tests/unit/runner.c \
            tests/unit/utest_e2e_helpers.c

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

# Stdlib bytecode flavor selection.  The tracked
# src/stdlib/urbi_stdlib_bytecode.gen.c is host-baked at f64
# (URBI_FLOAT_TYPE=8).  Cross targets built with a different URBI_FLOAT_TYPE
# need a per-target rebake — otherwise urbi_stdlib_boot fails silently
# inside urbi_vm_init (ULOAD_FLAVOR_MISMATCH on byte 13) and urbi_realm_create
# returns NULL because no stdlib protos got installed.
#
# Default (URBI_STDLIB_FLAVOR unset, e.g. host build): use the tracked .gen.c.
# Cross builds opt in by setting URBI_STDLIB_FLAVOR=N (matches URBI_FLOAT_TYPE
# numeric value).  The cross-* convenience targets that used to pass this
# automatically are parked (refound/core); an embedder driving their own
# cross toolchain sets URBI_STDLIB_FLAVOR=N on the command line directly.
#
# Bytecode-only targets never rebake — they only verify the freestanding
# symbol contract, the bake tool isn't built under URBI_BYTECODE_ONLY=1, and
# the f64 .gen.c is harmless data in that build.
ifeq ($(URBI_BYTECODE_ONLY),1)
  override URBI_STDLIB_FLAVOR :=
endif

ifeq ($(URBI_STDLIB_FLAVOR),)
  STDLIB_BYTECODE_GEN_C := src/stdlib/urbi_stdlib_bytecode.gen.c
else
  STDLIB_BYTECODE_GEN_C := $(BUILDDIR)/src/stdlib/urbi_stdlib_bytecode.gen.c
endif
STDLIB_BYTECODE_GEN_O := $(BUILDDIR)/src/stdlib/urbi_stdlib_bytecode.gen.o

OBJ := $(patsubst src/%.c,$(BUILDDIR)/src/%.o,$(SRC)) $(STDLIB_BYTECODE_GEN_O)
AUX_OBJS := $(patsubst src/%.c,$(BUILDDIR)/src/%.o,$(AUX_SRCS))
TEST_OBJ := $(patsubst tests/unit/%.c,$(BUILDDIR)/tests/unit/%.o,$(TEST_SRC))
LIB := $(BUILDDIR)/liburbi.a
LIBURBI_AUX := $(BUILDDIR)/liburbi_aux.a
RUNNER := $(BUILDDIR)/tests/unit/runner

# refound/core: the new runtime core (src/rt/) and its standalone test
# runner.  Empty today (src/rt/ holds only README.md); Task 2 onward adds
# src/rt/*.c and appends a suite + extern to tests/rt/runner.c.
RT_SRCS   := $(wildcard src/rt/*.c)
RT_OBJS   := $(patsubst %.c,$(BUILDDIR)/%.o,$(RT_SRCS))
RT_TEST_SRCS := $(wildcard tests/rt/test_*.c) tests/rt/runner.c
RT_LIB    := $(BUILDDIR)/liburbi-rt.a

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
CPPFLAGS += -Iinclude -Isrc -Itests/unit

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

all: $(LIB) $(LIBURBI_AUX) $(BUILDDIR)/urbi

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

# Aux layer archive — separate from $(LIB). Embedders link -laux at
# link time; liburbi.a contains zero aux symbols (nm-verified).
$(LIBURBI_AUX): $(AUX_OBJS)
	$(AR) rcs $@ $^

aux: $(LIBURBI_AUX)

$(RT_LIB): $(RT_OBJS)
	ar rcs $@ $^

$(BUILDDIR)/tests/rt/runner: $(RT_TEST_SRCS) $(RT_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Iinclude -Isrc -Itests/rt -o $@ $(RT_TEST_SRCS) $(RT_LIB) -lm

.PHONY: test-rt check-rt-layering
test-rt: $(BUILDDIR)/tests/rt/runner check-rt-layering
	$<
check-rt-layering:
	sh tests/scripts/check_rt_layering.sh

# Core archive without aux. Aux is hosted-only (uses <stdio.h>, etc.);
# cross-compile freestanding targets build `core` instead of `all` because
# bare-metal toolchains (e.g. Ubuntu's gcc-riscv64-unknown-elf) may not
# ship the libc headers aux depends on.
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

$(BUILDDIR)/tests/unit/%.o: tests/unit/%.c $(FLAGSTAMP)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(URBI_VIS_FLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

# tests/unit/test_detect_blob.c includes detect_blob.h from the eye_demo
# example's main/ directory.  Per-target CPPFLAGS append picks up the
# extra include path for just this TU; all other unit tests stay isolated
# from the example tree.
$(BUILDDIR)/tests/unit/test_detect_blob.o: CPPFLAGS += -Iexamples/esp32/eye_demo/main

# tests/unit/test_draw_crosshair.c includes crosshair.h from the same
# eye_demo main/ directory — same per-TU include-path pattern as
# test_detect_blob.o just above.
$(BUILDDIR)/tests/unit/test_draw_crosshair.o: CPPFLAGS += -Iexamples/esp32/eye_demo/main

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

# --- chk-host-driver ----------------------------------------------------
#
# chk-host-driver — bounded test host-driver for `.chk` fixtures whose
# observable needs an embedding-API operation the single-pass `urbi -i`
# REPL path cannot express (multi-realm isolation, urbi_step quiescence).
# Built into $(BUILDDIR) alongside `urbi` so each sanitizer variant
# (host-asan / host-ubsan) gets its own instrumented driver; run_chk.sh
# derives the driver path from the urbi-binary path it is handed.
# Test binary: links against liburbi.a + libm; includes private src/ headers.

$(BUILDDIR)/tests/integration:
	@mkdir -p $@

$(BUILDDIR)/tests/integration/chk_host_driver.o: tests/integration/chk_host_driver.c $(FLAGSTAMP) \
		| $(BUILDDIR)/tests/integration
	$(CC) $(CFLAGS) $(URBI_VIS_FLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

$(BUILDDIR)/chk-host-driver: $(BUILDDIR)/tests/integration/chk_host_driver.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $(BUILDDIR)/tests/integration/chk_host_driver.o $(LIB) -lm

chk-host-driver: $(BUILDDIR)/chk-host-driver

# --- Stdlib bake tool (host-only) ---------------------------------------
#
# tools/urbi-compile-stdlib is the Wave-2 build-time bake tool.  It
# walks src/stdlib/STDLIB_ORDER.txt, compiles each listed .u file via
# the public Urbi compile API, and emits the bytecode blob as
# src/stdlib/urbi_stdlib_bytecode.gen.c.
#
# HOST-ONLY: the bake tool always builds with native cc, never the
# cross toolchain.  Cross-arch builds consume the already-emitted
# .gen.c source (compiled for the target like any other src/stdlib/*.c).
# This keeps the chicken-and-egg out of the cross build: the bake
# runs once on the host, its output ships as portable C source.
#
# Cycle break:
#   The bake tool needs the urbi runtime to call urbi_compile_source,
#   but MUST NOT depend on .gen.o — that's the file it produces, and
#   the dep would form a build cycle:
#       liburbi.a → .gen.o → .gen.c → bake-tool → liburbi.a
#   So the bake tool links against host .o files DIRECTLY (excluding
#   .gen.o) plus a small stub (tools/stub_stdlib_bytecode.c) that
#   defines urbi_stdlib_bytecode[]=0 / urbi_stdlib_bytecode_len=0.
#   urbi_stdlib_boot gates on _len > 0 (see src/stdlib/stdlib_boot.c),
#   so the stub yields a clean no-op boot; the bake tool only needs
#   lex/parse/emit, not a populated stdlib.
#
# Two-pass stdlib bake (per delta spec §3.1):
#   1. liburbi.a builds with the committed .gen.c
#   2. tools/urbi-compile-stdlib links against host .o files + stub
#   3. .gen.c regenerates whenever a .u or STDLIB_ORDER.txt is newer,
#      and liburbi.a re-links from the regenerated .gen.o.

# Host-build pattern for the bake tool's deps: always native cc, so
# cross-arch sub-makes (TARGET=arm-*, TARGET=riscv-*, TARGET=host-asan,
# etc.) can still produce build/host/src/*.o for the host tool.
# Guarded by TARGET != host so it does NOT shadow the standard
# $(BUILDDIR)/src/%.o pattern when $(BUILDDIR) == build/host (default
# target) — same paths, same recipe, but a duplicate rule would emit
# a warning.
ifneq ($(TARGET),host)
build/host/src/%.o: src/%.c
	@mkdir -p $(@D)
	cc -std=c99 -Wall -Wextra -Wpedantic -Os -fvisibility=hidden -Iinclude -Isrc -MMD -MP -c -o $@ $<
endif

build/host/tools/stub_stdlib_bytecode.o: tools/stub_stdlib_bytecode.c
	@mkdir -p $(@D)
	cc -std=c99 -Os -Iinclude -Isrc -MMD -MP -c -o $@ $<

# T17 / Wave 1: bake-tool host source list must always include lex/parse/
# emit, regardless of URBI_BYTECODE_ONLY.  The bake tool runs at host build
# time and CALLS urbi_compile_source — both the symbol and the compiler
# frontend must be present.  Computed as a flag-independent enumeration of
# every src/**/*.c (matching the unfiltered $(SRC) expansion) minus the
# self-referential .gen.o.
HOST_BAKE_SRC := \
       $(filter-out $(AUX_SRCS),$(wildcard src/*.c)) \
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
       $(wildcard src/chunk/*.c) \
       $(wildcard src/value/*.c) \
       $(filter-out $(RUNTIME_PARKED_SRCS),$(wildcard src/runtime/*.c)) \
       $(wildcard src/realm/*.c) \
       $(wildcard src/object/*.c) \
       $(wildcard src/stdlib/*.c) \
       $(REPL_SRCS)
# NOTE: $(ROS2_SRCS) is deliberately NOT in the bake-tool source list.  The
# bake tool builds from flag-free build/host objects (the TARGET!=host rule),
# where stdlib_boot.o's urbi_ros_register call is #ifdef'd out — so the bake
# tool never references a ros symbol and does not need uros*.o.  Listing the
# ros objects here forces a bake-tool RELINK whenever they change, which (in a
# shared build/host populated by a prior URBI_ENABLE_REPL=1 TARGET=host build)
# pulls in stale REPL-flagged objects without REPL_SRCS in the link -> undefined
# refs (urepl_state_destroy / ujson_parse / urbi_introspect_*).  ROS2 is
# parked during the refound/core re-foundation (the dedicated non-host
# TARGET=host-ros2 target that used to isolate this build is gone, and
# URBI_ENABLE_ROS2=1 on bare TARGET=host still hard-errors above); this
# note stays for whoever re-attaches ROS2, so the TARGET=host
# stale-object trap (design-risk v0.12.0-H) isn't rediscovered the hard
# way.
HOST_BAKE_OBJ := $(filter-out build/host/src/stdlib/urbi_stdlib_bytecode.gen.o, \
                              $(patsubst src/%.c,build/host/src/%.o,$(HOST_BAKE_SRC)))
BAKE_STUB_O   := build/host/tools/stub_stdlib_bytecode.o

# T17 / Wave 1: the bake tool is a HOST-ONLY build-time helper.  Under
# URBI_BYTECODE_ONLY=1 the main $(SRC) excludes lex/parse/emit and
# urbi_compile_source becomes a header-gated absent symbol — neither of
# which the bake tool can use.  Solution: when URBI_BYTECODE_ONLY=1,
# don't try to (re)build the bake tool.  The committed
# src/stdlib/urbi_stdlib_bytecode.gen.c is consumed as-is.  Cross-arch
# bytecode-only builds never invoke the bake tool by design.
ifneq ($(URBI_BYTECODE_ONLY),1)
tools/urbi-compile-stdlib: tools/urbi-compile-stdlib.c $(HOST_BAKE_OBJ) $(BAKE_STUB_O)
	cc -std=c99 -Wall -Wextra -Wpedantic -Os \
	    -Iinclude -Isrc -o $@ $< $(HOST_BAKE_OBJ) $(BAKE_STUB_O) -lm

# Per-flavor bake tool variants — produce bytecode for a target with a
# different URBI_FLOAT_TYPE than the host (the default tool above is f64).
# Cross-compile targets that use f32 (-DURBI_FLOAT_TYPE=4) must bake their
# bytecode using `tools/urbi-compile-stdlib-f4`, otherwise the runtime will
# reject the module with ULOAD_FLAVOR_MISMATCH on byte 13.
#
# Pattern target: `tools/urbi-compile-stdlib-f4` builds a tool with
# -DURBI_FLOAT_TYPE=4.  Compiles all sources in one cc invocation rather
# than reusing build/host/*.o (which were compiled with f64).  ~10s build
# per flavor; cached after first build.
#
# Note: urbi_stdlib_bytecode.gen.c is filtered out (same as HOST_BAKE_OBJ
# above) — it defines urbi_stdlib_bytecode/_len symbols that also live in
# the stub.  We use the stub at link time to break the chicken-and-egg
# (the bake tool itself is what would normally regenerate the .gen.c).
tools/urbi-compile-stdlib-f%: tools/urbi-compile-stdlib.c \
        $(filter-out src/stdlib/urbi_stdlib_bytecode.gen.c,$(HOST_BAKE_SRC)) \
        tools/stub_stdlib_bytecode.c
	cc -std=c99 -Wall -Wextra -Wpedantic -Os -DURBI_FLOAT_TYPE=$* \
	    $(if $(filter 1,$(URBI_REPL_COOPERATIVE_ONLY)),-DURBI_REPL_COOPERATIVE_ONLY=1,) \
	    -Iinclude -Isrc -o $@ $^ -lm

# v0.9.4: tools/urbi-compile-stdlib-pico is a symlink to the f4 variant.
# Cortex-M0+ Pico uses URBI_FLOAT_TYPE=4 (float32), functionally identical
# to STM32F4.  The target-named symlink keeps the Pico example's CMakeLists
# invoking a target-named binary for clarity (and avoids hard-coding the
# floats convention into the example's build script).
tools/urbi-compile-stdlib-pico: tools/urbi-compile-stdlib-f4
	ln -sf urbi-compile-stdlib-f4 $@

# Two-pass stdlib bake (per delta §3.1):
# 1. liburbi.a builds with the placeholder .gen.c (committed in repo)
# 2. tools/urbi-compile-stdlib runs against intermediate liburbi.a
# 3. liburbi.a re-links with populated .gen.c
#
# The .gen.c rule depends on the bake tool + the order file + every
# .u under src/stdlib/.  Touching any of those triggers a rebake; the
# resulting .gen.c is then picked up by the existing src/stdlib/*.c
# wildcard, so liburbi.a re-links automatically.
#
# .gen.c is a TRACKED source file (not a generated artifact under
# build/) so the first build of liburbi.a does not require the bake
# tool — closing the chicken-and-egg between the tool and the library.

src/stdlib/urbi_stdlib_bytecode.gen.c: tools/urbi-compile-stdlib \
                                        src/stdlib/STDLIB_ORDER.txt \
                                        $(STDLIB_U_FILES)
	./tools/urbi-compile-stdlib \
	    src/stdlib/STDLIB_ORDER.txt \
	    src/stdlib \
	    $@
endif  # URBI_BYTECODE_ONLY != 1

# Shared with the per-target rebake rule below (must live outside the
# URBI_BYTECODE_ONLY guard so the rule body can expand it).
STDLIB_U_FILES := $(wildcard src/stdlib/*.u)
UROBOTICS_U_FILES := $(wildcard src/urobotics/*.u)

# Per-target stdlib rebake — fires only when URBI_STDLIB_FLAVOR is set
# (see commentary near the SRC/OBJ block).  Pattern rule
# $(BUILDDIR)/src/%.o: src/%.c does not match a source under $(BUILDDIR)/,
# so define both the .gen.c bake step and the .gen.o compile step
# explicitly.  Explicit rule with a recipe takes precedence over the
# pattern rule for the same target.
ifneq ($(URBI_STDLIB_FLAVOR),)
$(STDLIB_BYTECODE_GEN_C): tools/urbi-compile-stdlib-f$(URBI_STDLIB_FLAVOR) \
                          src/stdlib/STDLIB_ORDER.txt \
                          $(STDLIB_U_FILES)
	@mkdir -p $(@D)
	./tools/urbi-compile-stdlib-f$(URBI_STDLIB_FLAVOR) \
	    src/stdlib/STDLIB_ORDER.txt \
	    src/stdlib \
	    $@

$(STDLIB_BYTECODE_GEN_O): $(STDLIB_BYTECODE_GEN_C) $(FLAGSTAMP)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(URBI_VIS_FLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<
endif

# v0.12.2: bake the gated urobotics overlay into urbi_urobotics_bytecode.
# Host-only (default flavor); the tracked .gen.c is rebaked in place exactly
# like src/stdlib/urbi_stdlib_bytecode.gen.c.  Gated so the rule only exists
# when the overlay is enabled; the tracked 0-length placeholder covers the
# gate-off build.
ifeq ($(URBI_ENABLE_UROBOTICS),1)
src/urobotics/urobotics_bytecode.gen.c: tools/urbi-compile-stdlib \
                                        src/urobotics/UROBOTICS_ORDER.txt \
                                        $(UROBOTICS_U_FILES)
	./tools/urbi-compile-stdlib \
	    src/urobotics/UROBOTICS_ORDER.txt \
	    src/urobotics \
	    $@ \
	    urbi_urobotics_bytecode
endif

# --- Integration tests --------------------------------------------------
#
# test-integration runs the REPL shell harness against the built binary.
# Folded into the existing `test` aggregate so it runs under every
# sanitizer variant automatically. The shell script itself is NOT wrapped
# by $(RUNNER_WRAPPER) because dash's own "still-reachable" blocks break
# valgrind; the urbi binary is memory-clean when invoked directly.

test-integration: $(BUILDDIR)/urbi
	tests/integration/repl_smoke.sh $(BUILDDIR)/urbi

# v0.13.4: batch/embedding error-surfacing gate (B1/LANG4-14).  Exercises the
# -e / file entry points for uncaught-throw exit status (the chk suite only
# exercises the REPL path).
.PHONY: test-batch-errors
test-batch-errors: $(BUILDDIR)/urbi
	@URBI=$(BUILDDIR)/urbi bash tests/scripts/test-batch-errors.sh

# --- .chk conformance fixtures -----------------------------------------
#
# test-chk iterates all tests/chk/**/*.chk against the built urbi binary
# via tests/integration/run_chk.sh. One REPL session per fixture. Folded
# into `test` alongside test-integration so every sanitizer variant
# runs the fixtures automatically. Not valgrind-wrapped (same rationale
# as test-integration — urbi itself is memory-clean, and wrapping the
# sh+awk+sed pipeline adds noise, not signal).

# tests/chk/repl/*.chk are NDJSON fixtures (v0.9.1 Phase 8) for the REPL
# dispatcher, not urbiscript input consumable by run_chk.sh.  Their
# in-process driver was removed in the Phase 0 runtime-internals test
# cleanup; REPL is currently a parked feature pending v1.x re-attachment.
# Excluded here.
# refactor-3 CHK-01/04: per-outcome tally.  PASS(0) / SKIP(3, preset-gated) /
# PLACEHOLDER(4, annotated blocked:/deferred:/dropped: specification records)
# are healthy; VACUOUS(5, unannotated empty fixture) and FAIL(everything
# else) fail the suite.
test-chk: $(BUILDDIR)/urbi $(BUILDDIR)/chk-host-driver
	@pass=0; fail=0; skip=0; placeholder=0; vacuous=0; bad=""; \
	for f in $$(find tests/chk -path tests/chk/repl -prune -o -name '*.chk' -print 2>/dev/null | sort); do \
	    URBI_BUILD_PRESET=default tests/integration/run_chk.sh $(BUILDDIR)/urbi "$$f"; rc=$$?; \
	    case $$rc in \
	        0) pass=$$((pass + 1));; \
	        3) skip=$$((skip + 1));; \
	        4) placeholder=$$((placeholder + 1));; \
	        5) vacuous=$$((vacuous + 1)); bad="$$bad $$f";; \
	        *) fail=$$((fail + 1)); bad="$$bad $$f";; \
	    esac; \
	done; \
	echo "test-chk: $$pass passed, $$skip skipped (preset-gated), $$placeholder placeholders (blocked/deferred/dropped), $$vacuous vacuous-unannotated, $$fail failed"; \
	if [ $$fail -gt 0 ] || [ $$vacuous -gt 0 ]; then \
	    echo "test-chk: FAIL —$$bad"; \
	    exit 1; \
	fi; \
	if [ $$pass -eq 0 ]; then \
	    echo "test-chk: zero fixtures passed — corpus missing or runner broken"; \
	    exit 1; \
	fi

# refactor-3 CHK meta-gate: pins run_chk.sh's exit-code contract with stub
# binaries (no VM involved).  Must stay green across any future runner edit.
.PHONY: test-chk-runner
test-chk-runner:
	@bash tests/integration/test_run_chk_runner.sh

test: $(LIB) $(LIBURBI_AUX) $(TEST_OBJ) test-integration test-chk test-batch-errors test-rt check-rt-layering
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $(RUNNER) $(TEST_OBJ) $(LIBURBI_AUX) $(LIB) -lm
	$(RUNNER_WRAPPER) $(RUNNER)

# unit-runner — link the unit-test runner WITHOUT running it or the
# integration/chk gates.  Mirrors the link in `test`.
.PHONY: unit-runner
unit-runner: $(LIB) $(LIBURBI_AUX) $(TEST_OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $(RUNNER) $(TEST_OBJ) $(LIBURBI_AUX) $(LIB) -lm

.PHONY: test-wire-format-determinism
test-wire-format-determinism: $(BUILDDIR)/urbi
	@./tests/scripts/check_wire_format_determinism.sh

# Phase 9 (v0.7.1-embedding-api) aux-symbols gate.
# Asserts that liburbi.a (core) contains NO urbi_aux_* symbols.
# Aux functions live in liburbi_aux.a; leaking them into core breaks the
# aux governance contract (CONTRIBUTING.md "Aux layer governance") and
# the embedder's ability to strip the aux layer at link time.
.PHONY: test-aux-symbols
test-aux-symbols: $(LIB)
	@./scripts/check_aux_symbols.sh $(BUILDDIR)/liburbi.a

# API manifest gate — verifies that every urbi_ symbol exported from
# liburbi.a and liburbi_aux.a is enumerated in docs/api-surface-tiers.md.
# Catches new internal symbols accidentally becoming public and ensures the
# manifest stays in sync with the library.  Closes audit-1 F13 /
# api-ergonomics F12.  See tests/scripts/check-api-manifest.sh.
.PHONY: test-api-manifest
test-api-manifest: $(LIB) $(LIBURBI_AUX)
	@./tests/scripts/check-api-manifest.sh $(BUILDDIR)

# Embedding-guide code-sample drift detection — compiles every C block
# in docs/embedding-guide.md to catch API-signature drift.  Lightweight
# (<5 s); wired into releasetest Phase 1.  See
# tests/integration/test_embedding_guide_compiles.sh for the extraction
# and harness convention (STANDALONE vs FRAGMENT markers).
.PHONY: test-embedding-guide
test-embedding-guide: $(LIB) $(LIBURBI_AUX)
	@./tests/integration/test_embedding_guide_compiles.sh $(BUILDDIR)

# W5/v0.10.6: stdlib bytecode freshness gate (release F7).
# Regenerates the stdlib bytecode blob and diffs against the checked-in
# src/stdlib/urbi_stdlib_bytecode.gen.c.  Detects .u edits that were not
# followed by a re-bake commit.  Depends on the bake tool being built.
.PHONY: test-stdlib-bytecode-fresh
test-stdlib-bytecode-fresh: tools/urbi-compile-stdlib
	@./tests/scripts/check-stdlib-fresh.sh

# W2/v0.10.3: public-header self-containment gate.
# Compiles a minimal external program with ONLY -Iinclude (no -Isrc) to
# verify that include/urbi/gc.h and include/urbi/sched.h no longer pull in
# src/-prefixed internal headers.  Closes audit-1 F1 (completion).
.PHONY: test-external-embed-iinclude
test-external-embed-iinclude: $(LIB) $(LIBURBI_AUX)
	@./tests/integration/test_external_embed_iinclude.sh $(BUILDDIR)

# Phase 3 (v0.6.1-stdlib Wave 2) bake-tool determinism smoke gate.
# Runs tools/urbi-compile-stdlib three times against
# src/stdlib/STDLIB_ORDER.txt + src/stdlib/*.u and asserts that the
# three outputs are byte-identical.  Hard-fail in releasetest below.
# See tests/scripts/bake_smoke.sh.
.PHONY: test-bake-smoke
test-bake-smoke: tools/urbi-compile-stdlib
	@./tests/scripts/bake_smoke.sh
	@./tests/scripts/test_compile_stdlib_to_header.sh

# URBI_BYTECODE_ONLY smoke gate — originally a Phase 13 (v0.6.1-stdlib
# Wave 2) shape-only approximation; promoted at v0.7.0-c-api T15 to a
# real strip via the main Makefile (see COMPILER_FRONTEND_DIRS_EXCLUDED
# above).  This script still drives a standalone bypass-build to verify
# the architectural shape independently of the main Makefile and to
# confirm urbi_stdlib_boot / urbi_vm_init / urbi_vm_destroy /
# urbi_lock_heap remain exported after the strip.  Hard-fail in
# releasetest below.  See tests/scripts/build-bytecode-only.sh.
.PHONY: test-bytecode-only
test-bytecode-only:
	@./tests/scripts/build-bytecode-only.sh

# v0.9.3-ci-hardening: host-side freestanding gate.  Compiles each
# URBI_BYTECODE_ONLY-eligible TU under host cc with -ffreestanding
# -DURBI_BYTECODE_ONLY=1 + nm-greps each .o against the forbidden-
# libc regex (printf/snprintf/malloc/free/…).  Catches the leak
# class that masked v0.9.1 + v0.9.2 from CI without requiring any
# cross toolchain.  See tests/scripts/build-freestanding-host.sh.
.PHONY: test-freestanding-host
test-freestanding-host:
	@./tests/scripts/build-freestanding-host.sh

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

# refactor-3 TEST-GAP-03: -O2 build variant.  The matrix was -Os/-O0/-O1
# only; the v0.10.11 channel_proto bug was -Os-specific, proving the suite
# is optimization-level sensitive.  Runs the full unit+integration+chk
# aggregate at the optimization level desktop embedders actually use.
test-o2:
	$(MAKE) TARGET=host-o2 \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O2 -g" \
		test

# --- Determinism gate -------------------------------------------------------
#
# test-determinism builds and runs the full unit-test suite 100 times under
# the default preset, verifying that urbi_get_determinism_checksum() returns
# a stable value across runs.
#
# The full runner is invoked each iteration (no per-suite filter exists in
# runner.c); at ~15-25ms per run, 100 invocations take ~1.5-2.5 seconds.
# Any non-zero exit from the runner fails the gate with the iteration number.
#
# Enables -DURBI_DEBUG=1 because the determinism checksum function is
# guarded by #ifdef URBI_DEBUG.

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

test-determinism: test-determinism-default
	@echo "=== Determinism gate: default preset × 100 runs PASS ==="

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
	bash tests/integration/test_full_corpus_sanitize.sh

# --- Release test aggregate --------------------------------------------
#
# releasetest runs every host-side gate the CI matrix runs, in parallel.
# Cross-compile, REPL-server, ROS2, urobotics, trace, perf-counters, and
# mem-debug gates are parked (refound/core) and excluded — see
# REPL_PARKED_SRCS / RUNTIME_PARKED_SRCS above.
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
    test-gc-stress \
    lint docs-check coverage test-stress test-gc-none-build \
    test-scan-build test-cppcheck test-tidy-strict \
    test-wire-format-determinism \
    test-bake-smoke test-bytecode-only test-freestanding-host \
    test-api-manifest test-aux-symbols \
    test-embedding-guide test-external-embed-iinclude \
    test-stdlib-bytecode-fresh \
    test-chk-runner test-fuzz-smoke test-o2
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
	@echo "=== releasetest: pre-fanout regeneration (serialized; refactor-3 BLD-02c) ==="
	@$(MAKE) --no-print-directory tools/urbi-compile-stdlib src/stdlib/urbi_stdlib_bytecode.gen.c
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
	$(CC) $(CFLAGS) $(STRESS_CPPFLAGS) $< -L$(BUILDDIR) -lurbi -lm -o $@

$(STRESS_BUILDDIR)/gc_many_cycles: tests/stress/gc_many_cycles.c $(LIB) | $(STRESS_BUILDDIR)
	$(CC) $(CFLAGS) $(STRESS_CPPFLAGS) $< -L$(BUILDDIR) -lurbi -lm -o $@

$(STRESS_BUILDDIR)/gc_pause_time: tests/stress/gc_pause_time.c $(LIB) | $(STRESS_BUILDDIR)
	$(CC) $(CFLAGS) $(STRESS_CPPFLAGS) $< -L$(BUILDDIR) -lurbi -lm -o $@

$(STRESS_BUILDDIR)/gc_barrier_throughput: tests/stress/gc_barrier_throughput.c $(LIB) | $(STRESS_BUILDDIR)
	$(CC) $(CFLAGS) $(STRESS_CPPFLAGS) $< -L$(BUILDDIR) -lurbi -lm -o $@

$(STRESS_BUILDDIR)/stress_event_emit_loop: tests/stress/stress_event_emit_loop.c $(LIB) | $(STRESS_BUILDDIR)
	$(CC) $(CFLAGS) $(STRESS_CPPFLAGS) -Isrc $< -L$(BUILDDIR) -lurbi -lm -o $@

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
	    $< -L$(BUILDDIR) -lurbi -lm -o $@

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

# refactor-3 TEST-GAP-02 fix: $(SRC) filters out the stdlib bytecode .gen.c
# (the OBJ list adds its object separately), so passing bare $(SRC) here had
# bit-rotted the fuzz link ("undefined reference to urbi_stdlib_bytecode_len").
FUZZ_SRC := $(SRC) src/stdlib/urbi_stdlib_bytecode.gen.c

$(FUZZ_BUILDDIR)/fuzz_lex: tests/fuzz/fuzz_lex.c $(FUZZ_SRC) | $(FUZZ_BUILDDIR)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(CPPFLAGS) -o $@ $(FUZZ_SRC) tests/fuzz/fuzz_lex.c -lm

$(FUZZ_BUILDDIR)/fuzz_parse: tests/fuzz/fuzz_parse.c $(FUZZ_SRC) | $(FUZZ_BUILDDIR)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(CPPFLAGS) -o $@ $(FUZZ_SRC) tests/fuzz/fuzz_parse.c -lm

$(FUZZ_BUILDDIR)/fuzz_vm: tests/fuzz/fuzz_vm.c $(FUZZ_SRC) | $(FUZZ_BUILDDIR)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(CPPFLAGS) -o $@ $(FUZZ_SRC) tests/fuzz/fuzz_vm.c -lm

# refactor-3 TEST-GAP-02: the network-facing JSON parsers.  Both TUs are
# libc-self-contained, so the harness links exactly those two sources.
$(FUZZ_BUILDDIR)/fuzz_json: tests/fuzz/fuzz_json.c src/repl/ujson.c src/repl/urepl_ndjson.c | $(FUZZ_BUILDDIR)
	$(FUZZ_CC) $(FUZZ_CFLAGS) $(CPPFLAGS) -o $@ \
	    tests/fuzz/fuzz_json.c src/repl/ujson.c src/repl/urepl_ndjson.c

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

fuzz-json: fuzz-tools $(FUZZ_BUILDDIR)/fuzz_json
	@echo "running fuzz_json (Ctrl-C to stop; use -runs=N for bounded)"
	$(FUZZ_BUILDDIR)/fuzz_json

fuzz-chunk: fuzz-tools $(FUZZ_BUILDDIR)/fuzz_chunk
	@echo "running fuzz_chunk (Ctrl-C to stop; use -runs=N for bounded)"
	$(FUZZ_BUILDDIR)/fuzz_chunk tests/fuzz/seeds/chunk/

fuzz-build: fuzz-tools $(FUZZ_BUILDDIR)/fuzz_lex $(FUZZ_BUILDDIR)/fuzz_parse $(FUZZ_BUILDDIR)/fuzz_vm $(FUZZ_BUILDDIR)/fuzz_json $(FUZZ_BUILDDIR)/fuzz_chunk

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
	@$(MAKE) --no-print-directory $(FUZZ_BUILDDIR)/fuzz_lex $(FUZZ_BUILDDIR)/fuzz_parse $(FUZZ_BUILDDIR)/fuzz_vm $(FUZZ_BUILDDIR)/fuzz_json $(FUZZ_BUILDDIR)/fuzz_chunk
	$(FUZZ_BUILDDIR)/fuzz_lex   -runs=20000 -max_total_time=120
	$(FUZZ_BUILDDIR)/fuzz_parse -runs=20000 -max_total_time=120
	$(FUZZ_BUILDDIR)/fuzz_vm    -runs=20000 -max_total_time=120
	$(FUZZ_BUILDDIR)/fuzz_json  -runs=20000 -max_total_time=120
	$(FUZZ_BUILDDIR)/fuzz_chunk -runs=20000 -max_total_time=120 tests/fuzz/seeds/chunk/
	@echo "test-fuzz-smoke: 5 harnesses x 20000 bounded runs clean"

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
	# Line-coverage floor lowered from 85% to 80% in Phase 0 of the core re-foundation:
	# the deleted runtime-internals unit tests covered code that the refound/core
	# branch replaces. Re-baseline for the new core when it lands.
	gcovr --root . \
	      --object-directory build/host-coverage \
	      --filter 'src/' \
	      --merge-mode-functions=merge-use-line-min \
	      --fail-under-line 80 \
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
	rm -f tools/urbi-compile-stdlib tools/urbi-compile-stdlib-pico \
	      tools/urbi-compile-stdlib-f[0-9]*

# bake-clean — force the bake tool to regenerate
# src/stdlib/urbi_stdlib_bytecode.gen.c from STDLIB_ORDER.txt + .u files.
#
# Routine builds do not need this — the dep-graph picks up .u changes
# automatically.  Use this when the committed .gen.c drifts from what
# the current sources would produce (e.g. a .u was edited but `make`
# did not notice because the file timestamp regressed).
#
# Distinct from `make clean` — it does not touch build/ at all, only
# the tracked .gen.c source.
bake-clean: tools/urbi-compile-stdlib
	./tools/urbi-compile-stdlib \
	    src/stdlib/STDLIB_ORDER.txt \
	    src/stdlib \
	    src/stdlib/urbi_stdlib_bytecode.gen.c

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
# strings, and include/urbi/version.h all agree with the latest git tag.
# Run by `make check-version-sync` and by the version-sync GHA job.
#
# TODO (Wave 1 merge): add check-version-sync as a dep of docs-check once
# the W1 README refresh lands on the integration branch and the README ABI /
# wire / tag strings match the version.h + uchunk.h values.

check-version-sync:
	@tests/scripts/check-version-sync.sh

.PHONY: all aux core test test-asan test-ubsan test-debug test-switch test-determinism test-determinism-default clean bake-clean compile_commands.json tidy tidy-fix test-tidy-strict cppcheck test-cppcheck test-scan-build analyzer lint docs-check docs-check-tools check-version-sync coverage coverage-tools test-valgrind valgrind-tools fuzz-lex fuzz-parse fuzz-vm fuzz-build fuzz-tools urbi-bin test-integration test-chk releasetest _releasetest_phase1 _releasetest_phase2 test-stress test-gc-none-build test-gc-pause test-bake-smoke test-bytecode-only test-freestanding-host test-api-manifest test-aux-symbols test-embedding-guide test-external-embed-iinclude test-stdlib-bytecode-fresh test-gc-stress unit-runner test-chk-runner test-fuzz-smoke test-o2 fuzz-json force-flagstamp
