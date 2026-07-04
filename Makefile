# Aux layer — separate translation unit, separate archive. Filtered out
# of the core SRC list below so liburbi.a (core) stays free of aux symbols.
# Embedders opt into the aux layer by linking -laux at link time. See
# CONTRIBUTING.md "Aux layer governance" and include/urbi/aux.h.
AUX_SRCS := src/urbi_aux.c

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
  REPL_SRCS := $(wildcard src/repl/*.c)
  # v0.9.4-followup: cooperative-only filter (Pico, bare-metal STM32, etc.)
  # When URBI_REPL_COOPERATIVE_ONLY=1, drop the POSIX-only TUs: TCP/Unix/PTY
  # listener (pthread + eventfd + sockets) and socket transports. Embedder
  # drives serve_step from main loop; no listener thread needed.
  ifeq ($(URBI_REPL_COOPERATIVE_ONLY),1)
    CPPFLAGS += -DURBI_REPL_COOPERATIVE_ONLY=1
    REPL_SRCS := $(filter-out \
        src/repl/urepl_transport_tcp.c \
        src/repl/urepl_transport_unix.c \
        src/repl/urepl_transport_pty.c \
        src/repl/urepl_auth.c, \
        $(REPL_SRCS))
  endif
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
       $(wildcard src/runtime/*.c) \
       $(wildcard src/realm/*.c) \
       $(wildcard src/object/*.c) \
       $(filter-out src/stdlib/urbi_stdlib_bytecode.gen.c,$(wildcard src/stdlib/*.c)) \
       $(REPL_SRCS) \
       $(ROS2_SRCS) \
       $(UROBOTICS_SRCS)
TEST_SRC := $(wildcard tests/unit/test_*.c) tests/unit/runner.c \
            tests/unit/twatcher_install_helper.c \
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
    $(error URBI_ENABLE_ROS2=1 on bare TARGET=host is forbidden (stale-object trap v0.12.0-H). Use the dedicated targets: `make test-ros2` / `make test-ros-urobotics`, or pass an explicit TARGET=host-ros2)
  endif
  ifeq ($(URBI_ENABLE_UROBOTICS),1)
    $(error URBI_ENABLE_UROBOTICS=1 on bare TARGET=host is forbidden (stale-object trap v0.12.0-H). Use the dedicated targets: `make test-urobotics` / `make test-ros-urobotics`, or pass an explicit TARGET=host-urobotics)
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
# numeric value).  The recursive cross-arm / cross-riscv / cross-stm32f4
# targets pass URBI_STDLIB_FLAVOR=4 automatically.
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
sinclude $(shell find build -name '*.d' 2>/dev/null)

all: $(LIB) $(LIBURBI_AUX) $(BUILDDIR)/urbi

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

# Aux layer archive — separate from $(LIB). Embedders link -laux at
# link time; liburbi.a contains zero aux symbols (nm-verified).
$(LIBURBI_AUX): $(AUX_OBJS)
	$(AR) rcs $@ $^

aux: $(LIBURBI_AUX)

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

# urbi-trace — the urbi CLI built with URBI_TRACE=1 so --trace/--trace-out work
# (the default CLI is trace-off, so its trace control API resolves to no-op
# stubs).  Built into build/host-trace via a recursive make with the trace
# flag, mirroring the test-trace pattern; the binary lands at
# build/host-trace/urbi.
.PHONY: urbi-trace
urbi-trace:
	$(MAKE) TARGET=host-trace \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -DURBI_TRACE=1" \
		urbi-bin
	@echo "urbi-trace: built build/host-trace/urbi (URBI_TRACE=1)"

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

# --- v0.9.1 REPL CLIs (URBI_ENABLE_REPL=1 only) ------------------------
#
# urbi-server: headless network REPL daemon — boots a UVM, optionally
#   runs a boot script, registers the TCP transport, drives urbi_step()
#   until SIGINT/SIGTERM.  Links against liburbi.a + libm.
#
# urbi-send: NDJSON client utility — pure POSIX sockets + libc.  Does
#   NOT link against liburbi.  Gated behind URBI_ENABLE_REPL=1 only to
#   avoid maintaining a binary nobody can talk to (server side disabled).

ifeq ($(URBI_ENABLE_REPL),1)
URBI_SERVER := $(BUILDDIR)/urbi-server
URBI_SEND   := $(BUILDDIR)/urbi-send

$(BUILDDIR)/tools/urbi-server.o: tools/urbi-server.c $(FLAGSTAMP) | $(BUILDDIR)/tools
	$(CC) $(CFLAGS) $(URBI_VIS_FLAGS) $(CPPFLAGS) -Itools -MMD -MP -c -o $@ $<

$(URBI_SERVER): $(BUILDDIR)/tools/urbi-server.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $(BUILDDIR)/tools/urbi-server.o $(LIB) -lm

$(BUILDDIR)/tools/urbi-send.o: tools/urbi-send.c $(FLAGSTAMP) | $(BUILDDIR)/tools
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(URBI_SEND): $(BUILDDIR)/tools/urbi-send.o
	$(CC) $(CFLAGS) -o $@ $(BUILDDIR)/tools/urbi-send.o

urbi-server-bin: $(URBI_SERVER)
urbi-send-bin:   $(URBI_SEND)

all: $(URBI_SERVER) $(URBI_SEND)
endif

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
       $(wildcard src/runtime/*.c) \
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
# refs (urepl_state_destroy / ujson_parse / urbi_introspect_*).  Build ros via
# `make test-ros2` / TARGET=host-ros2, never URBI_ENABLE_ROS2=1 on TARGET=host
# (design-risk v0.12.0-H).
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

# v0.9.1: urbi-server end-to-end smoke (URBI_ENABLE_REPL=1 only).  Spins
# up the daemon on a high port, runs `1+2` via NDJSON, expects the
# `"value":"3"` envelope back, then SIGTERMs the daemon.  Uses python3
# as the TCP client; skips cleanly if python3 is missing.
ifeq ($(URBI_ENABLE_REPL),1)
test-urbi-server-smoke: $(URBI_SERVER)
	BUILD=$(BUILDDIR) tests/integration/urbi_server_smoke.sh
else
test-urbi-server-smoke:
	@echo "test-urbi-server-smoke: URBI_ENABLE_REPL=0; skipping"
endif

# --- .chk conformance fixtures -----------------------------------------
#
# test-chk iterates all tests/chk/**/*.chk against the built urbi binary
# via tests/integration/run_chk.sh. One REPL session per fixture. Folded
# into `test` alongside test-integration so every sanitizer variant
# runs the fixtures automatically. Not valgrind-wrapped (same rationale
# as test-integration — urbi itself is memory-clean, and wrapping the
# sh+awk+sed pipeline adds noise, not signal).

# tests/chk/repl/*.chk are NDJSON fixtures (v0.9.1 Phase 8) driven in-
# process by tests/unit/test_repl_chk_corpus.c, not by run_chk.sh which
# expects urbiscript input.  Excluded here.
# refactor-3 CHK-01/04: per-outcome tally.  PASS(0) / SKIP(3, preset-gated;
# covered by test-chk-ros + test-chk-urobotics + test-chk-ros-urobotics) /
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

# test-chk-ros — runs all tests/chk/ros/*.chk under URBI_BUILD_PRESET=ros.
# Every fixture must RUN and PASS; a SKIP is a gate failure (the vacuous-
# fixture trap: preset mismatch silently empties coverage).
.PHONY: test-chk-ros
test-chk-ros: $(BUILDDIR)/urbi $(BUILDDIR)/chk-host-driver
	@count=0; \
	for f in tests/chk/ros/*.chk; do \
	    count=$$((count + 1)); \
	    out=$$(URBI_BUILD_PRESET=ros tests/integration/run_chk.sh $(BUILDDIR)/urbi "$$f" 2>&1); rc=$$?; \
	    echo "$$out"; \
	    if [ $$rc -ne 0 ]; then \
	        echo "test-chk-ros: FAIL — $$f rc=$$rc under preset ros (SKIP/placeholder counts as failure here)"; \
	        exit 1; \
	    fi; \
	done; \
	echo "$$count ros chk fixture(s) ran + passed under preset ros"

test: $(LIB) $(LIBURBI_AUX) $(TEST_OBJ) test-integration test-chk test-urbi-server-smoke
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $(RUNNER) $(TEST_OBJ) $(LIBURBI_AUX) $(LIB) -lm
	$(RUNNER_WRAPPER) $(RUNNER)

# unit-runner — link the unit-test runner WITHOUT running it or the
# integration/chk gates.  Used by the GDB smoke gate (test-gdb) to produce a
# debug (-O0 -g) inferior with readable symbols.  Mirrors the link in `test`.
.PHONY: unit-runner
unit-runner: $(LIB) $(LIBURBI_AUX) $(TEST_OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $(RUNNER) $(TEST_OBJ) $(LIBURBI_AUX) $(LIB) -lm
# Note: test-port-stm32f4 used to be in the line above but was pulled out to
# avoid a parallel-make race - it builds host-side stub binaries into a
# fixed `build/port_stm32f4/` path with no $(TARGET) suffix, so every
# releasetest Phase 1 variant (test / test-asan / test-ubsan / test-debug
# / test-switch) raced to write the same binary, producing intermittent
# "Text file busy" / "Permission denied" failures under -j. Phase 1 now
# invokes test-port-stm32f4 once as a separate gate.

# v0.8.2: host-side unit tests for STM32F4 port shims, using mock BSP.
# Each test compiles a single port shim TU against the mock BSP layer.
# URBI_PORT_TEST=1 selects the mock-include path in the port shim TUs.
PORT_STM32F4_TESTS := \
	test_port_allocator \
	test_port_time \
	test_port_writer \
	test_port_diag \
	test_port_lcd \
	test_port_gyro \
	test_port_button

PORT_STM32F4_TEST_DEPS_COMMON := \
	tests/port_stm32f4/mock_bsp.c \
	src/runtime/uabi_guards.c

PORT_STM32F4_CFLAGS := -std=c99 -Wall -Wextra \
	-DURBI_PORT_TEST=1 \
	-I tests/port_stm32f4 \
	-I include \
	-I components/stm32f4-hal-baremetal/include

# Each test gets its own binary in build/port_stm32f4/
build/port_stm32f4/test_port_allocator: tests/port_stm32f4/test_port_allocator.c \
	$(PORT_STM32F4_TEST_DEPS_COMMON) \
	components/stm32f4-hal-baremetal/src/port/port_allocator.c
	@mkdir -p $(@D)
	$(CC) $(PORT_STM32F4_CFLAGS) $^ -o $@

build/port_stm32f4/test_port_time: tests/port_stm32f4/test_port_time.c \
	$(PORT_STM32F4_TEST_DEPS_COMMON) \
	components/stm32f4-hal-baremetal/src/port/port_time.c
	@mkdir -p $(@D)
	$(CC) $(PORT_STM32F4_CFLAGS) $^ -o $@

build/port_stm32f4/test_port_writer: tests/port_stm32f4/test_port_writer.c \
	$(PORT_STM32F4_TEST_DEPS_COMMON) \
	components/stm32f4-hal-baremetal/src/port/port_writer.c
	@mkdir -p $(@D)
	$(CC) $(PORT_STM32F4_CFLAGS) $^ -o $@

build/port_stm32f4/test_port_diag: tests/port_stm32f4/test_port_diag.c \
	$(PORT_STM32F4_TEST_DEPS_COMMON) \
	components/stm32f4-hal-baremetal/src/port/port_writer.c \
	components/stm32f4-hal-baremetal/src/port/port_diag.c
	@mkdir -p $(@D)
	$(CC) $(PORT_STM32F4_CFLAGS) $^ -o $@

build/port_stm32f4/test_port_lcd: tests/port_stm32f4/test_port_lcd.c \
	$(PORT_STM32F4_TEST_DEPS_COMMON) \
	components/stm32f4-hal-baremetal/src/port/port_lcd.c \
	$(LIB)
	@mkdir -p $(@D)
	$(CC) $(PORT_STM32F4_CFLAGS) $^ -lm -o $@

build/port_stm32f4/test_port_gyro: tests/port_stm32f4/test_port_gyro.c \
	$(PORT_STM32F4_TEST_DEPS_COMMON) \
	components/stm32f4-hal-baremetal/src/port/port_gyro.c \
	$(LIB)
	@mkdir -p $(@D)
	$(CC) $(PORT_STM32F4_CFLAGS) $^ -lm -o $@

build/port_stm32f4/test_port_button: tests/port_stm32f4/test_port_button.c \
	$(PORT_STM32F4_TEST_DEPS_COMMON) \
	components/stm32f4-hal-baremetal/src/port/port_button.c
	@mkdir -p $(@D)
	$(CC) $(PORT_STM32F4_CFLAGS) $^ -o $@

.PHONY: test-port-stm32f4
test-port-stm32f4: $(addprefix build/port_stm32f4/, $(PORT_STM32F4_TESTS))
	@for t in $^; do echo "Running $$t..."; $$t || exit 1; done
	@echo "All STM32F4 port tests PASS"

.PHONY: test-loc-cap
test-loc-cap:
	@./tests/scripts/check_loc_cap.sh

# T34 (v0.7.0 Wave 1): GC roots-coverage gate.  Asserts every UVAL_*
# enum value declared in include/urbi/types.h is referenced at least
# once under src/gc/.  Closes the bug class that surfaced as the
# M4-era UVAL_OBJECT / UVAL_EVENT shading gap (fixed inline at v0.6.2
# Phase 6).  Hard-fail in releasetest Phase 1 below.
.PHONY: test-gc-roots-coverage
test-gc-roots-coverage:
	@./tests/scripts/check-gc-roots-coverage.sh

.PHONY: test-wire-format-determinism
test-wire-format-determinism: $(BUILDDIR)/urbi
	@./tests/scripts/check_wire_format_determinism.sh

# Phase 21 (v0.5.8-cleanup) gate: every public-API and subsystem-public
# header function declaration must carry an immediately-preceding /* ... */
# comment.  Hard-fail in releasetest below.  See
# tests/scripts/check_docstring_coverage.sh for the cascade rules.
.PHONY: test-docstring-coverage
test-docstring-coverage:
	@./tests/scripts/check_docstring_coverage.sh

# Phase 9 (v0.7.1-embedding-api) aux-symbols gate.
# Asserts that liburbi.a (core) contains NO urbi_aux_* symbols.
# Aux functions live in liburbi_aux.a; leaking them into core breaks the
# aux governance contract (CONTRIBUTING.md "Aux layer governance") and
# the embedder's ability to strip the aux layer at link time.
# Inverse of the Wave-1 freestanding gate (test-freestanding).
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

# W3/v0.10.6: wire-format freeze gate — verifies that the _Static_assert
# in src/chunk/uchunk_io.c, the macros in src/chunk/uchunk.h, and the
# version reference in docs/internals/bytecode-format.md are all in sync.
# Closes bytecode-pipeline F1 (completion).
.PHONY: test-wire-freeze
test-wire-freeze:
	@./tests/scripts/check-wire-freeze.sh

# Embedding-guide code-sample drift detection — compiles every C block
# in docs/embedding-guide.md to catch API-signature drift.  Lightweight
# (<5 s); wired into releasetest Phase 1.  See
# tests/integration/test_embedding_guide_compiles.sh for the extraction
# and harness convention (STANDALONE vs FRAGMENT markers).
.PHONY: test-embedding-guide
test-embedding-guide: $(LIB) $(LIBURBI_AUX)
	@./tests/integration/test_embedding_guide_compiles.sh $(BUILDDIR)

# W2/v0.10.6: ABI freeze pin belt-and-braces gate.
# Asserts that the _Static_assert in include/urbi/version.h matches the
# URBI_API_VERSION_* macros.  Detects accidental drift; the static_assert
# is the primary catch (compile-time), this gate makes intent visible in CI.
.PHONY: test-abi-freeze
test-abi-freeze:
	@./tests/scripts/check-abi-freeze.sh

# W5/v0.10.6: stdlib bytecode freshness gate (release F7).
# Regenerates the stdlib bytecode blob and diffs against the checked-in
# src/stdlib/urbi_stdlib_bytecode.gen.c.  Detects .u edits that were not
# followed by a re-bake commit.  Depends on the bake tool being built.
.PHONY: test-stdlib-bytecode-fresh
test-stdlib-bytecode-fresh: tools/urbi-compile-stdlib
	@./tests/scripts/check-stdlib-fresh.sh

# W5/v0.10.6: dependency-pin static check (release F14).
# Parses CI for Docker/SDK/toolchain version pins and compares them to
# docs/reference/embedded-port-sources.md.  Catches CI/docs drift without
# needing network access or a live toolchain.
.PHONY: test-dependency-pins
test-dependency-pins:
	@./tests/scripts/check-dependency-pins.sh

# v0.11.2: host trace-tooling decoder unit test.  Runs the Python URBT decoder
# (tools/urbi-trace-decode.py) against constructed dumps and asserts the
# Chrome Trace JSON.  Skips cleanly if python3 is missing (host-only gate).
.PHONY: test-trace-decode
test-trace-decode:
	@sh tests/scripts/check-trace-decode.sh

# v0.11.2: host trace-tooling end-to-end gate.  Builds the URBI_TRACE=1 CLI,
# captures a tiny run to a URBT dump, decodes it, and asserts valid Chrome
# Trace JSON.  Skips cleanly if python3 is missing.
.PHONY: test-trace-capture
# refactor-3 BLD-02a: depend on test-trace (same build/host-trace tree, same
# CFLAGS string) instead of urbi-trace, so releasetest Phase 1's -j fanout
# cannot run two recursive makes into build/host-trace concurrently.
# test-trace's recursive `make test` builds build/host-trace/urbi as a side
# effect (test-chk prerequisite), which is the binary the capture script uses.
test-trace-capture: test-trace
	@sh tests/scripts/check-trace-capture.sh

# v0.11.2: GDB walker smoke gate.  Loads tools/gdb/urbi.py against a debug
# (-O0 -g) unit runner and asserts the walkers run without a Python error.
# Skips cleanly if gdb is missing (net-new tooling in this repo).
.PHONY: test-gdb
test-gdb:
	@sh tests/scripts/check-gdb.sh

# test-gdb-memdebug — GDB owner-tag walkers against a -DURBI_MEM_DEBUG=1 runner.
# Asserts urbi-allocs surfaces allocation sites (owner sidecar populated).
.PHONY: test-gdb-memdebug
test-gdb-memdebug:
	@MEMDBG=1 sh tests/scripts/check-gdb.sh

# W4/v0.10.6: REPL security gate aggregate.
# Runs all repl_security_* and repl_oom_paths tests via the unit-test runner.
# Wired into RELEASETEST_PHASE1; also runs standalone for CI cost budgeting.
# Builds with URBI_ENABLE_REPL=1 so the REPL TUs and security test suites
# are compiled in (the suites are guarded by #ifdef URBI_ENABLE_REPL).
.PHONY: test-repl-security
test-repl-security:
	$(MAKE) TARGET=host-repl-security \
		URBI_ENABLE_REPL=1 \
		test

# v0.12.0: ROS2 bridge (mock) gate.  Builds the ros-enabled host binary,
# runs the full unit suite (which includes ros bridge unit tests), then
# runs all tests/chk/ros/*.chk under preset ros so no fixture is skipped.
# Own TARGET= keeps Phase 1 -j parallelism race-free.
.PHONY: test-ros2
test-ros2:
	$(MAKE) TARGET=host-ros2 URBI_ENABLE_ROS2=1 \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g" \
		test test-chk-ros

# v0.12.0: ROS2 codegen + gate script targets.  Mirror the test-gdb pattern
# (delegate entirely to a script; skip-if-absent logic lives in the script).
.PHONY: check-ros-gate
check-ros-gate:
	@sh tests/scripts/check-ros-gate.sh

.PHONY: check-rosgen
check-rosgen:
	@sh tests/scripts/check-rosgen.sh

.PHONY: check-rosgen-determinism
check-rosgen-determinism:
	@sh tests/scripts/check-rosgen-determinism.sh

# test-chk-urobotics — runs all tests/chk/urobotics/*.chk under
# URBI_BUILD_PRESET=urobotics.  Every fixture must RUN and PASS; a SKIP is a
# gate failure (the vacuous-fixture trap: preset mismatch silently empties
# coverage).
.PHONY: test-chk-urobotics
test-chk-urobotics: $(BUILDDIR)/urbi $(BUILDDIR)/chk-host-driver
	@count=0; \
	for f in tests/chk/urobotics/*.chk; do \
	    count=$$((count + 1)); \
	    out=$$(URBI_BUILD_PRESET=urobotics tests/integration/run_chk.sh $(BUILDDIR)/urbi "$$f" 2>&1); rc=$$?; \
	    echo "$$out"; \
	    if [ $$rc -ne 0 ]; then \
	        echo "test-chk-urobotics: FAIL — $$f rc=$$rc under preset urobotics (SKIP/placeholder counts as failure here)"; \
	        exit 1; \
	    fi; \
	done; \
	echo "$$count urobotics chk fixture(s) ran + passed under preset urobotics"

# v0.12.2: Standard Robotics API overlay gate.  Builds the overlay-enabled host
# binary (TARGET=host-urobotics — never URBI_ENABLE_UROBOTICS=1 on bare
# TARGET=host: the build/host stale-object collision, design-trap v0.12.0-H),
# runs the unit suite, then all tests/chk/urobotics/*.chk under preset
# urobotics so no fixture is silently skipped.
.PHONY: test-urobotics
test-urobotics:
	$(MAKE) TARGET=host-urobotics URBI_ENABLE_UROBOTICS=1 \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g" \
		test test-chk-urobotics

.PHONY: check-urobotics-determinism
check-urobotics-determinism: tools/urbi-compile-stdlib
	@sh tests/scripts/check-urobotics-determinism.sh

# test-chk-ros-urobotics — runs all tests/chk/ros-urobotics/*.chk under
# URBI_BUILD_PRESET=ros-urobotics.  Every fixture must RUN and PASS; a SKIP is
# a gate failure (the vacuous-fixture trap: preset mismatch silently empties
# coverage).
.PHONY: test-chk-ros-urobotics
test-chk-ros-urobotics: $(BUILDDIR)/urbi $(BUILDDIR)/chk-host-driver
	@count=0; \
	for f in tests/chk/ros-urobotics/*.chk; do \
	    count=$$((count + 1)); \
	    out=$$(URBI_BUILD_PRESET=ros-urobotics tests/integration/run_chk.sh $(BUILDDIR)/urbi "$$f" 2>&1); rc=$$?; \
	    echo "$$out"; \
	    if [ $$rc -ne 0 ]; then \
	        echo "test-chk-ros-urobotics: FAIL — $$f rc=$$rc under preset ros-urobotics (SKIP/placeholder counts as failure here)"; \
	        exit 1; \
	    fi; \
	done; \
	echo "$$count ros-urobotics chk fixture(s) ran + passed under preset ros-urobotics"

# v0.12.3: facet<->ROS2 binding gate.  Builds with BOTH optional components on
# (TARGET=host-ros-urobotics — never the flags on bare TARGET=host: v0.12.0-H),
# runs the unit suite, then all tests/chk/ros-urobotics/*.chk under the combined
# preset so no binding fixture is silently skipped.
.PHONY: test-ros-urobotics
test-ros-urobotics:
	$(MAKE) TARGET=host-ros-urobotics URBI_ENABLE_ROS2=1 URBI_ENABLE_UROBOTICS=1 \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g" \
		test test-chk-ros-urobotics

# B1/v0.12.1: Docker ros:jazzy integration harness.
# Builds a derived image (copy-in only — never a host mount), compiles the
# grounding spike inside the container, and asserts "PUBSUB got=42".
# Skipped automatically when docker is absent.
.PHONY: ros-integration
ros-integration:
	@command -v docker >/dev/null 2>&1 || { echo "ros-integration: docker not found — SKIP"; exit 0; }
	@docker build -q -t urbi-ros-jazzy tests/integration/ros/ >/dev/null
	@cid=$$(docker create --rm -w /src urbi-ros-jazzy bash tests/integration/ros/run-integration.sh); \
	 docker cp . $$cid:/src; \
	 docker start -a $$cid

# W2/v0.10.3: public-header self-containment gate.
# Compiles a minimal external program with ONLY -Iinclude (no -Isrc) to
# verify that include/urbi/gc.h and include/urbi/sched.h no longer pull in
# src/-prefixed internal headers.  Closes audit-1 F1 (completion).
.PHONY: test-external-embed-iinclude
test-external-embed-iinclude: $(LIB) $(LIBURBI_AUX)
	@./tests/integration/test_external_embed_iinclude.sh $(BUILDDIR)

# v1.0 (M10 / B1): fresh-clone build of every shipped-port example.  Proves a
# pristine tree (no stale build/) builds the Linux REPL + each cross firmware.
# ESP32-S3 skips cleanly when IDF_PATH is unset.  Advisory (not in releasetest's
# core gate set) because it depends on cross toolchains; see
# docs/release/clone-build-demo.md.
.PHONY: clone-build-demo-check
clone-build-demo-check: ## Fresh-clone build of all shipped-port examples
	bash tests/scripts/clone-build-demo.sh

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

# v0.6.2 Wave 3 oracle-diff — third-party sanity check against urbiforge
# 3.x (CMake-built, installed at $(URBI_ORACLE_ROOT)).  Diffs our urbi
# binary's stdout against the urbiforge engine (`urbi-launch -s --`)
# for selected legacy fixtures.  NOT a CI gate — opt-in for Wave 3
# parity validation on Gaps #1 + #4.
URBI_ORACLE_ROOT ?= /tmp/urbi-oracle

.PHONY: oracle-diff
oracle-diff: $(BUILDDIR)/urbi
	@if [ ! -x "$(URBI_ORACLE_ROOT)/bin/urbi-launch" ]; then \
	    echo "oracle-diff: $(URBI_ORACLE_ROOT)/bin/urbi-launch not built; see Phase 0 Task 4 of v0.6.2 plan"; \
	    exit 1; \
	fi
	@URBI_ORACLE_ROOT=$(URBI_ORACLE_ROOT) bash tests/scripts/oracle-diff.sh $(ORACLE_FIXTURES)

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

# test-trace — full suite under URBI_TRACE=1 (trace subsystem compiled in,
# all channels default-OFF).  Verifies the trace build is green and that the
# ring / tracepoints / bring-up primitives / Debug.trace marker behave.  Own
# TARGET= so Phase 1 -j parallelism stays race-free.
.PHONY: test-trace
test-trace:
	$(MAKE) TARGET=host-trace \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -DURBI_TRACE=1" \
		test

# test-trace-compiled-out — proves the URBI_TRACE-OFF archive (default $(LIB))
# carries no trace ring/emit internals.  The control-API stubs are present in
# both modes by design and are excluded from the forbidden list.
.PHONY: test-trace-compiled-out
test-trace-compiled-out: $(LIB)
	@sh tests/scripts/check-trace-compiled-out.sh $(LIB)

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

# test-determinism-trace — the default preset with URBI_TRACE=1 added.  Proves
# a trace-enabled build stays deterministic across 100 runs: trace channels
# default OFF, so no records are emitted and the checksummed observable state
# is unperturbed by compiling the subsystem in.  CI-only (not in releasetest).
test-determinism-trace:
	$(MAKE) TARGET=host-determinism-trace \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -DURBI_DEBUG=1 -DURBI_TRACE=1" \
		test
	@echo "=== Determinism gate: trace preset (100 runs) ==="
	@for i in $$(seq 1 100); do \
	    build/host-determinism-trace/tests/unit/runner > /dev/null \
	    || { echo "FAIL on iteration $$i (trace preset)"; exit 1; }; \
	done
	@echo "=== Trace preset: 100 runs PASS (URBI_TRACE=1 stays deterministic) ==="

# test-perf-counters — full suite under URBI_PERF_COUNTERS=1 (per-opcode/slot
# counters compiled in; per-event GC counters are always-on).  Verifies the
# perf build is green and that the counters increment + Debug.profile() seam
# behave.  Own TARGET= so Phase 1 -j parallelism stays race-free.
.PHONY: test-perf-counters
test-perf-counters:
	$(MAKE) TARGET=host-perf \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -DURBI_PERF_COUNTERS=1" \
		test

# test-determinism-perf — the default preset with URBI_PERF_COUNTERS=1 added.
# Proves a perf-counter build stays deterministic across 100 runs: ALL counters
# (and GC timing) are excluded from urbi_get_determinism_checksum, so the
# checksummed observable state is unperturbed.  This is the same URBI_DEBUG +
# extra-define combination that surfaced the v0.11.0 CI-only fault, so it runs
# in CI even though UPerfCounters is small + embedded (no on-stack hazard).
# CI-only (not in releasetest).
test-determinism-perf:
	$(MAKE) TARGET=host-determinism-perf \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -DURBI_DEBUG=1 -DURBI_PERF_COUNTERS=1" \
		test
	@echo "=== Determinism gate: perf preset (100 runs) ==="
	@for i in $$(seq 1 100); do \
	    build/host-determinism-perf/tests/unit/runner > /dev/null \
	    || { echo "FAIL on iteration $$i (perf preset)"; exit 1; }; \
	done
	@echo "=== Perf preset: 100 runs PASS (counters excluded from checksum) ==="

# test-mem-debug — full suite under URBI_MEM_DEBUG=1 (owner tags, trailing
# redzone, poison-on-free + quarantine, handle/pin leak detection).  Own
# TARGET= so Phase 1 -j parallelism stays race-free.
.PHONY: test-mem-debug
test-mem-debug:
	$(MAKE) TARGET=host-memdbg \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O0 -g -DURBI_MEM_DEBUG=1" \
		test

# test-gc-stress — refactor-3 TEST-GAP-01: full suite under URBI_GC_STRESS=1
# (synchronous full collection before EVERY GC-cell allocation — the
# highest-leverage detector for the rooting-gap bug class: catch_value
# v0.11.4, walk_uevent v1.0 hang, container elements B2).  -O1 keeps
# wall-clock tolerable.  Own TARGET= so Phase 1 -j parallelism stays
# race-free.  In RELEASETEST_PHASE1 since v0.13.2 (corpus green; ~2 min
# wall-clock solo, comparable to test-mem-debug).
.PHONY: test-gc-stress
test-gc-stress:
	$(MAKE) TARGET=host-gc-stress \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -DURBI_GC_STRESS=1" \
		test

# test-determinism-memdebug — the URBI_DEBUG + URBI_MEM_DEBUG combo.  Proves the
# determinism checksum is unperturbed by the memdbg substate (alloc_seq, owner
# pointers, poison/quarantine/redzone state are all excluded) AND doubles as the
# UVM-layout-perturbation canary (the same class that surfaced the v0.11.0
# CI-only fault).  CI-only (not in releasetest).
test-determinism-memdebug:
	$(MAKE) TARGET=host-determinism-memdbg \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -DURBI_DEBUG=1 -DURBI_MEM_DEBUG=1" \
		test
	@echo "=== Determinism gate: mem-debug preset (100 runs) ==="
	@for i in $$(seq 1 100); do \
	    build/host-determinism-memdbg/tests/unit/runner > /dev/null \
	    || { echo "FAIL on iteration $$i (mem-debug preset)"; exit 1; }; \
	done
	@echo "=== mem-debug preset: 100 runs PASS (memdbg excluded from checksum) ==="

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
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O0 -g -DURBI_SKIP_THREAD_FUZZ_TESTS=1" \
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
    test-trace test-trace-compiled-out test-perf-counters \
    test-trace-decode test-trace-capture test-gdb \
    test-mem-debug test-gdb-memdebug test-gc-stress \
    lint docs-check coverage test-stress test-gc-none-build \
    test-scan-build test-cppcheck test-tidy-strict \
    test-wire-format-determinism test-docstring-coverage \
    test-bake-smoke test-bytecode-only test-freestanding-host \
    test-gc-roots-coverage test-api-manifest test-aux-symbols \
    test-embedding-guide test-external-embed-iinclude test-port-stm32f4 \
    test-abi-freeze test-wire-freeze test-repl-security \
    test-stdlib-bytecode-fresh test-dependency-pins \
    test-ros2 check-ros-gate check-rosgen check-rosgen-determinism \
    test-urobotics check-urobotics-determinism test-ros-urobotics \
    test-chk-runner test-fuzz-smoke test-o2
# Phase 2: valgrind, running alone after Phase 1 finishes.
# ros-integration is excluded from releasetest (container-only; needs docker).
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
	@detect() { \
	     cc="$$1"; \
	     command -v "$$cc" >/dev/null 2>&1 || { echo absent; return; }; \
	     tmpc=$$(mktemp --suffix=.c); \
	     tmpo=$$(mktemp --suffix=.o); \
	     printf '#include <string.h>\nint main(void){return 0;}\n' > "$$tmpc"; \
	     if "$$cc" -c -o "$$tmpo" "$$tmpc" 2>/dev/null; then \
	         rm -f "$$tmpc" "$$tmpo"; echo present; \
	     else \
	         rm -f "$$tmpc" "$$tmpo"; echo broken; \
	     fi; \
	 }; \
	 arm=$$(detect arm-none-eabi-gcc); \
	 riscv=$$(detect riscv-none-elf-gcc); \
	 esp=$$(detect xtensa-esp-elf-gcc); \
	 echo "=== releasetest: cross-toolchain detection ==="; \
	 phase0=""; \
	 if [ "$$arm" = present ]; then \
	     echo "  arm-none-eabi-gcc    : present  -> cross-arm + cross-stm32f4 + cross-pico + cross-pico-repl + test-freestanding(arm,stm32f4,pico) included"; \
	     echo "    (test-cross-pico-freestanding-golden runs only under GHA - golden is captured against GHA's apt arm-none-eabi-gcc 13.2.1, image bake uses xpack 14.2.1; symbol set differs by ~1 libgcc helper. See design-risks: 'switch GHA ARM jobs to xpack')"; \
	     echo "    (test-cross-pico-repl-elf NOT in Phase 0 - SKIP path would mask CI regressions; run explicitly when working on Pico REPL)"; \
	     phase0="$$phase0 cross-arm-bytecode-only cross-stm32f4-bytecode-only cross-pico-bytecode-only cross-pico-repl"; \
	 elif [ "$$arm" = broken ]; then \
	     echo "  arm-none-eabi-gcc    : broken   -> sysroot missing; skipped (install xpack via docs/cross-toolchain-setup.md)"; \
	 else \
	     echo "  arm-none-eabi-gcc    : absent   -> cross-arm + cross-stm32f4 skipped (GHA CI authoritative)"; \
	 fi; \
	 if [ "$$riscv" = present ]; then \
	     echo "  riscv-none-elf-gcc   : present  -> cross-riscv + test-freestanding(riscv) included"; \
	     phase0="$$phase0 cross-riscv-bytecode-only"; \
	 elif [ "$$riscv" = broken ]; then \
	     echo "  riscv-none-elf-gcc   : broken   -> sysroot missing; skipped (install xpack via docs/cross-toolchain-setup.md)"; \
	 else \
	     echo "  riscv-none-elf-gcc   : absent   -> cross-riscv skipped (GHA CI authoritative)"; \
	 fi; \
	 if [ "$$esp" = present ]; then \
	     echo "  xtensa-esp-elf-gcc   : present  -> cross-esp32s3-bytecode-only + freestanding-golden included"; \
	     phase0="$$phase0 cross-esp32s3-bytecode-only test-cross-esp32s3-freestanding-golden"; \
	 elif [ "$$esp" = broken ]; then \
	     echo "  xtensa-esp-elf-gcc   : broken   -> sysroot missing; skipped (install xpack via docs/cross-toolchain-setup.md)"; \
	 else \
	     echo "  xtensa-esp-elf-gcc   : absent   -> cross-esp32s3 + freestanding-golden skipped (GHA CI authoritative)"; \
	 fi; \
	 echo "For full local parity install xpack toolchains - see docs/cross-toolchain-setup.md."; \
	 if [ -n "$$phase0" ]; then \
	     echo "=== releasetest: Phase 0 (cross, sequential):$$phase0 ==="; \
	     phase0_start=$$(date +%s); \
	     $(MAKE) --no-print-directory $$phase0 || exit $$?; \
	     for tc_archive in $$(echo "$$phase0" | tr ' ' '\n' | grep -E 'cross-(arm|stm32f4|riscv)-bytecode-only' | awk '{print "build/" $$0 "/liburbi.a"}'); do \
	         sh tests/scripts/test-freestanding.sh "$$tc_archive" || exit $$?; \
	     done; \
	     phase0_end=$$(date +%s); \
	     echo "=== releasetest: Phase 0 passed ($$((phase0_end - phase0_start)) s) ==="; \
	 fi
	@echo "=== releasetest: pre-fanout regeneration (serialized; refactor-3 BLD-02c) ==="
	@$(MAKE) --no-print-directory tools/urbi-compile-stdlib src/stdlib/urbi_stdlib_bytecode.gen.c
	@$(MAKE) --no-print-directory URBI_ENABLE_UROBOTICS=1 TARGET=host-urobotics src/urobotics/urobotics_bytecode.gen.c
	@$(MAKE) --no-print-directory URBI_ENABLE_ROS2=1 TARGET=host-ros2 src/ros/generated/ros_msgs.gen.c src/ros/generated/ros_msgs.gen.h
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

fuzz-build: fuzz-tools $(FUZZ_BUILDDIR)/fuzz_lex $(FUZZ_BUILDDIR)/fuzz_parse $(FUZZ_BUILDDIR)/fuzz_vm $(FUZZ_BUILDDIR)/fuzz_json

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
	@$(MAKE) --no-print-directory $(FUZZ_BUILDDIR)/fuzz_lex $(FUZZ_BUILDDIR)/fuzz_parse $(FUZZ_BUILDDIR)/fuzz_vm $(FUZZ_BUILDDIR)/fuzz_json
	$(FUZZ_BUILDDIR)/fuzz_lex   -runs=20000 -max_total_time=120
	$(FUZZ_BUILDDIR)/fuzz_parse -runs=20000 -max_total_time=120
	$(FUZZ_BUILDDIR)/fuzz_vm    -runs=20000 -max_total_time=120
	$(FUZZ_BUILDDIR)/fuzz_json  -runs=20000 -max_total_time=120
	@echo "test-fuzz-smoke: 4 harnesses x 20000 bounded runs clean"

fuzz-tools:
	@command -v $(FUZZ_CC) >/dev/null 2>&1 || { \
	    echo "error: $(FUZZ_CC) not found in PATH"; \
	    echo "install: sudo apt-get install -y clang libclang-rt-18-dev"; \
	    exit 1; \
	}

# Cross-compile sanity (builds liburbi.a only; no test runner).
cross-arm:
	$(MAKE) TARGET=arm-cortex-m7 \
		URBI_STDLIB_FLAVOR=4 \
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
		core

cross-riscv:
	$(MAKE) TARGET=riscv-rv32imc \
		URBI_STDLIB_FLAVOR=4 \
		CC=riscv64-unknown-elf-gcc \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os -march=rv32imc -mabi=ilp32 -ffreestanding \
		        -DURBI_FLOAT_TYPE=4 \
		        -DURBI_WATCHER_POOL_SIZE=64" \
		AR=riscv64-unknown-elf-ar \
		core

# v0.8.2: cross-compile for STM32F4 (Cortex-M4F).  Same arm-none-eabi
# toolchain as cross-arm; differs in -mcpu and FPU flags.
#
# UVM_STACK_CAP override: default 2048 slots × 16 B = 32 KB per strand
# register stack is too big for a 1 MB SDRAM heap with frequent watcher-
# body spawns (gyro_tick @ 50 ms).  512 slots × 16 B = 8 KB lets ~100+
# alive strands coexist, eliminating the OOM bursts from the v0.8.2
# bring-up.  Mandelbrot demo functions are shallow enough (~5 nested
# calls × ~10 locals each) that 512 slots is comfortable; complex
# embeddings can override per-build.
cross-stm32f4:
	$(MAKE) TARGET=arm-cortex-m4 \
		URBI_STDLIB_FLAVOR=4 \
		CC=arm-none-eabi-gcc \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os \
		        -mcpu=cortex-m4 -mthumb \
		        -mfpu=fpv4-sp-d16 -mfloat-abi=hard \
		        -ffreestanding \
		        -DURBI_CLEANUP_MAX=16 \
		        -DURBI_STRAND_BUDGET_MAX=200 \
		        -DURBI_GC_SLICE_BUDGET=2048 \
		        -DURBI_WATCHER_POOL_SIZE=16 \
		        -DURBI_WATCHER_READSET_MAX=4 \
		        -DURBI_EVENT_RING_DEPTH=32 \
		        -DURBI_FLOAT_TYPE=4 \
		        -DUVM_STACK_CAP=512" \
		AR=arm-none-eabi-ar \
		core

# v0.9.4: cross-compile for Raspberry Pi Pico (RP2040 / Cortex-M0+ / armv6-m).
# Same arm-none-eabi toolchain as cross-arm; differs in -mcpu (no FPU,
# no integer divide, libgcc soft-float helpers in play).
#
# UVM_STACK_CAP=512 mirrors cross-stm32f4 — Pico's 264 KB SRAM is even
# tighter, so the same 8 KB register-stack-per-strand cap applies.
cross-pico:
	$(MAKE) TARGET=arm-cortex-m0plus \
		URBI_STDLIB_FLAVOR=4 \
		URBI_ENABLE_REPL=$(URBI_ENABLE_REPL) \
		URBI_REPL_COOPERATIVE_ONLY=$(if $(filter 1,$(URBI_ENABLE_REPL)),1,) \
		CC=arm-none-eabi-gcc \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os \
		        -mcpu=cortex-m0plus -mthumb -mfloat-abi=soft \
		        -ffreestanding \
		        -DURBI_CLEANUP_MAX=16 \
		        -DURBI_STRAND_BUDGET_MAX=200 \
		        -DURBI_GC_SLICE_BUDGET=2048 \
		        -DURBI_WATCHER_POOL_SIZE=16 \
		        -DURBI_WATCHER_READSET_MAX=4 \
		        -DURBI_EVENT_RING_DEPTH=32 \
		        -DURBI_FLOAT_TYPE=4 \
		        -DUVM_STACK_CAP=512" \
		AR=arm-none-eabi-ar \
		core

# T19 / Wave 1: URBI_BYTECODE_ONLY=1 variants of the cross-arch builds.
# Used by `make test-freestanding` (T18) to verify the freestanding subset
# contract on the embedded targets (no hosted libc fallthrough).
#
# Distinct TARGET names give each variant its own $(BUILDDIR) tree
# (build/cross-arm-bytecode-only/, build/cross-riscv-bytecode-only/), so
# `make cross-arm cross-arm-bytecode-only` can coexist without rebuild
# churn.  URBI_BYTECODE_ONLY=1 propagates through the recursive $(MAKE)
# invocation (-DURBI_BYTECODE_ONLY=1 reaches the CFLAGS+CPPFLAGS append
# in the top-of-Makefile gate).
cross-arm-bytecode-only:
	$(MAKE) URBI_BYTECODE_ONLY=1 \
		TARGET=cross-arm-bytecode-only \
		CC=arm-none-eabi-gcc \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os -mcpu=cortex-m7 -mthumb -ffreestanding \
		        -DURBI_BYTECODE_ONLY=1 \
		        -DURBI_CLEANUP_MAX=16 \
		        -DURBI_STRAND_BUDGET_MAX=200 \
		        -DURBI_GC_SLICE_BUDGET=2048 \
		        -DURBI_WATCHER_POOL_SIZE=16 \
		        -DURBI_WATCHER_READSET_MAX=4 \
		        -DURBI_EVENT_RING_DEPTH=32 \
		        -DURBI_FLOAT_TYPE=4" \
		AR=arm-none-eabi-ar \
		core

cross-riscv-bytecode-only:
	$(MAKE) URBI_BYTECODE_ONLY=1 \
		TARGET=cross-riscv-bytecode-only \
		CC=riscv64-unknown-elf-gcc \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os -march=rv32imc -mabi=ilp32 -ffreestanding \
		        -DURBI_BYTECODE_ONLY=1 \
		        -DURBI_FLOAT_TYPE=4 \
		        -DURBI_WATCHER_POOL_SIZE=64" \
		AR=riscv64-unknown-elf-ar \
		core

# v0.8.2: STM32F4 bytecode-only freestanding variant.
cross-stm32f4-bytecode-only:
	$(MAKE) URBI_BYTECODE_ONLY=1 \
		TARGET=cross-stm32f4-bytecode-only \
		CC=arm-none-eabi-gcc \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os \
		        -mcpu=cortex-m4 -mthumb \
		        -mfpu=fpv4-sp-d16 -mfloat-abi=hard \
		        -ffreestanding \
		        -DURBI_BYTECODE_ONLY=1 \
		        -DURBI_CLEANUP_MAX=16 \
		        -DURBI_STRAND_BUDGET_MAX=200 \
		        -DURBI_GC_SLICE_BUDGET=2048 \
		        -DURBI_WATCHER_POOL_SIZE=16 \
		        -DURBI_WATCHER_READSET_MAX=4 \
		        -DURBI_EVENT_RING_DEPTH=32 \
		        -DURBI_FLOAT_TYPE=4" \
		AR=arm-none-eabi-ar \
		core
	@sh tests/scripts/test-freestanding.sh build/cross-stm32f4-bytecode-only/liburbi.a

# v0.9.4: bytecode-only variant of cross-pico — freestanding-clean
# archive check ensures no libc symbols leak when URBI_BYTECODE_ONLY=1.
cross-pico-bytecode-only:
	$(MAKE) URBI_BYTECODE_ONLY=1 \
		TARGET=cross-pico-bytecode-only \
		CC=arm-none-eabi-gcc \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os \
		        -mcpu=cortex-m0plus -mthumb -mfloat-abi=soft \
		        -ffreestanding \
		        -DURBI_BYTECODE_ONLY=1 \
		        -DURBI_FLOAT_TYPE=4" \
		AR=arm-none-eabi-ar \
		core
	@sh tests/scripts/test-freestanding.sh build/cross-pico-bytecode-only/liburbi.a

# v0.9.4-followup: cooperative-only REPL build for Pi Pico. Composes
# cross-pico with URBI_REPL_COOPERATIVE_ONLY=1 + URBI_ENABLE_REPL=1.
# Distinct TARGET so build dir doesn't clobber the non-REPL cross-pico
# build at build/arm-cortex-m0plus/. Locks in the portability work
# via test-cross-pico-repl-elf below.
cross-pico-repl:
	$(MAKE) TARGET=arm-cortex-m0plus-repl \
		URBI_STDLIB_FLAVOR=4 \
		URBI_ENABLE_REPL=1 \
		URBI_REPL_COOPERATIVE_ONLY=1 \
		CC=arm-none-eabi-gcc \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os \
		        -mcpu=cortex-m0plus -mthumb -mfloat-abi=soft \
		        -ffreestanding \
		        -DURBI_CLEANUP_MAX=16 \
		        -DURBI_STRAND_BUDGET_MAX=200 \
		        -DURBI_GC_SLICE_BUDGET=2048 \
		        -DURBI_WATCHER_POOL_SIZE=16 \
		        -DURBI_WATCHER_READSET_MAX=4 \
		        -DURBI_EVENT_RING_DEPTH=32 \
		        -DURBI_FLOAT_TYPE=4 \
		        -DUVM_STACK_CAP=512" \
		AR=arm-none-eabi-ar \
		core
	@arm-none-eabi-size --totals build/arm-cortex-m0plus-repl/liburbi.a | tail -1

# T10 / Wave 2: ESP32-S3 (Xtensa LX7) bytecode-only cross-build.
# Uses the unified ESP-IDF v6.0.1+ toolchain (xtensa-esp-elf-{gcc,ar,nm});
# target ISA selection happens via `-mlongcalls` (the ESP32 Xtensa marker).
# Footprint -D set mirrors cross-arm-bytecode-only — ESP32-S3 has a
# comparable RAM envelope to the Cortex-M7 target.  Inline freestanding
# gate matches the spec §4.7 contract.
cross-esp32s3-bytecode-only:
	$(MAKE) URBI_BYTECODE_ONLY=1 \
		TARGET=cross-esp32s3-bytecode-only \
		CC=xtensa-esp-elf-gcc \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os -mlongcalls -ffreestanding \
		        -DURBI_BYTECODE_ONLY=1 \
		        -DURBI_CLEANUP_MAX=16 \
		        -DURBI_STRAND_BUDGET_MAX=200 \
		        -DURBI_GC_SLICE_BUDGET=2048 \
		        -DURBI_WATCHER_POOL_SIZE=16 \
		        -DURBI_WATCHER_READSET_MAX=4 \
		        -DURBI_EVENT_RING_DEPTH=32 \
		        -DURBI_FLOAT_TYPE=4" \
		AR=xtensa-esp-elf-ar \
		core
	@sh tests/scripts/test-freestanding.sh build/cross-esp32s3-bytecode-only/liburbi.a

# T11 / Wave 2: ESP32-S3 (Xtensa LX7) full cross-build (lex/parse/emit
# included).  Mirrors the cross-arm / cross-riscv shape — no
# URBI_BYTECODE_ONLY=1 and no inline freestanding gate, since full mode
# pulls in the compiler front-end which may surface hosted-libc deps for
# diagnostics.  Uses the unified ESP-IDF v6.0.1+ toolchain
# (xtensa-esp-elf-{gcc,ar,nm}); target ISA selection via `-mlongcalls`.
cross-esp32s3-full:
	$(MAKE) TARGET=cross-esp32s3-full \
		CC=xtensa-esp-elf-gcc \
		CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Os -mlongcalls -ffreestanding" \
		AR=xtensa-esp-elf-ar \
		core

# T12 / Wave 2: ESP32-S3 bytecode-only freestanding-signature golden gate.
# Tighter than the hardcoded-libc forbidden list in test-freestanding.sh:
# pins the FULL set of truly-unresolved (archive-level) symbols against a
# golden.  Any NEW unresolved symbol — even one not in the hardcoded list —
# trips the gate, surfacing latent dependency drift (e.g. a newly-introduced
# libgcc helper, or accidental leakage of time() / strncmp() / etc. behind
# a missed __STDC_HOSTED__ guard).  To update the golden after verifying
# intent: delete tests/golden/v0.7.2-esp32-nm-bytecode-only.txt and
# re-run this target; the FAIL diff doubles as the regeneration command.
# NOT wired into releasetest — toolchain availability isn't universal;
# CI invokes this from the cross-compile workflow (see T13).
.PHONY: test-cross-esp32s3-freestanding-golden
test-cross-esp32s3-freestanding-golden: cross-esp32s3-bytecode-only
	@actual=$$(mktemp); \
	 xtensa-esp-elf-nm build/cross-esp32s3-bytecode-only/liburbi.a 2>/dev/null \
	  | awk 'NF >= 3 && $$3 !~ /:$$/ && $$1 != "U" {defined[$$3]=1} \
	         NF >= 2 && $$1 == "U" {undefined[$$2]=1} \
	         END {for (s in undefined) if (!(s in defined)) print s}' \
	  | sort -u > "$$actual"; \
	 if diff -u tests/golden/v0.7.2-esp32-nm-bytecode-only.txt "$$actual"; then \
	     echo "PASS: cross-esp32s3-bytecode-only freestanding signature matches golden"; \
	     rm -f "$$actual"; \
	 else \
	     echo "FAIL: cross-esp32s3-bytecode-only freestanding signature drifted from golden."; \
	     echo "      Either fix the leak or update the golden after verifying intent:"; \
	     echo "        cp $$actual tests/golden/v0.7.2-esp32-nm-bytecode-only.txt"; \
	     exit 1; \
	 fi

# v0.9.4: cross-pico freestanding signature golden — mirror of the
# esp32s3 gate above.  Locks the symbol surface of the cortex-m0plus
# bytecode-only archive against drift.  Not wired into releasetest by
# default (the existing v0.9.3 probe-compile dispatcher includes it
# conditionally — Task 2.2 wires that).  On FAIL, the recipe prints
# the regeneration command.
.PHONY: test-cross-pico-freestanding-golden
test-cross-pico-freestanding-golden: cross-pico-bytecode-only
	@actual=$$(mktemp); \
	 arm-none-eabi-nm build/cross-pico-bytecode-only/liburbi.a 2>/dev/null \
	  | awk 'NF >= 3 && $$3 !~ /:$$/ && $$1 != "U" {defined[$$3]=1} \
	         NF >= 2 && $$1 == "U" {undefined[$$2]=1} \
	         END {for (s in undefined) if (!(s in defined)) print s}' \
	  | sort -u > "$$actual"; \
	 if diff -u tests/golden/v0.9.4-pico-nm-bytecode-only.txt "$$actual"; then \
	     echo "PASS: cross-pico-bytecode-only freestanding signature matches golden"; \
	     rm -f "$$actual"; \
	 else \
	     echo "FAIL: cross-pico-bytecode-only freestanding signature drifted from golden."; \
	     echo "      Either fix the leak or update the golden after verifying intent:"; \
	     echo "        cp $$actual tests/golden/v0.9.4-pico-nm-bytecode-only.txt"; \
	     exit 1; \
	 fi

# v0.9.4-followup: example .elf link gate. Builds liburbi.a (cooperative
# REPL) + the repl_demo Pico example to verify the embedding API surface
# stays linkable end-to-end. Requires pico-sdk at $$PICO_SDK_PATH or
# vendored at workspace-root tools/pico-sdk; SKIPs if absent (CI clones
# it explicitly before invoking this target).
# NOT wired into Phase 0 — the SKIP path would mask CI regressions when
# pico-sdk is absent; local devs without the SDK should run releasetest
# without false reds. Run explicitly or from CI when working on Pico REPL.
.PHONY: test-cross-pico-repl-elf
test-cross-pico-repl-elf: cross-pico-repl tools/urbi-compile-stdlib-pico
	@if [ -z "$$PICO_SDK_PATH" ] && [ ! -d "../tools/pico-sdk" ]; then \
	    echo "SKIP: PICO_SDK_PATH unset and ../tools/pico-sdk absent"; \
	    exit 0; \
	fi
	@PSP="$${PICO_SDK_PATH:-$$PWD/../tools/pico-sdk}"; \
	 cmlog=$$(mktemp); mklog=$$(mktemp); \
	 cd examples/pico/repl_demo && \
	 mkdir -p build && cd build && \
	 cmake -DPICO_SDK_PATH="$$PSP" \
	       -DLIBURBI_BUILD_SUBDIR=arm-cortex-m0plus-repl \
	       .. > "$$cmlog" 2>&1 || \
	     { cat "$$cmlog"; exit 1; }; \
	 $(MAKE) repl_demo > "$$mklog" 2>&1 || \
	     { echo "--- CMake output ---"; cat "$$cmlog"; \
	       echo "--- make output ---"; cat "$$mklog"; exit 1; }; \
	 rm -f "$$cmlog" "$$mklog"
	@arm-none-eabi-size build/arm-cortex-m0plus-repl/liburbi.a \
	                    examples/pico/repl_demo/build/repl_demo.elf \
	                    | tail -2
	@echo "PASS: cross-pico-repl example .elf links cleanly"

# BLD-CI-3: STM32F4 mandelbrot app compile gate.  Builds the full application
# ELF (HAL + BSP + urbi port shims + liburbi.a) with arm-none-eabi-gcc.
# Catches app-level breakage invisible to the library-only cross-stm32f4 job:
# public header regressions, internal header changes used by main.c (vm/uvm.h,
# chunk/uchunk.h), and URBI_FLOAT_TYPE mismatch.
#
# Requires STM32CubeF4 v1.28.2 at ../tools/stm32cube-f4 (sibling peer checkout;
# see docs/reference/embedded-port-sources.md).  Not wired into releasetest —
# the external HAL dependency makes it ill-suited as a default local gate.
# CI provisions the HAL before invoking this target.
.PHONY: test-cross-stm32f4-app
test-cross-stm32f4-app: cross-stm32f4
	$(MAKE) -C examples/stm32f4/mandelbrot
	@echo "stm32f4 mandelbrot app: OK"

# T18 / Wave 1: freestanding CI gate.  Asserts cross-arch URBI_BYTECODE_ONLY=1
# liburbi.a archives have no unresolved hosted-libc symbols (printf, malloc,
# fopen, etc.).  Depends on cross-arm-bytecode-only and cross-riscv-bytecode-only
# (T19).  Not wired into `releasetest` — the cross-toolchain dependency makes
# it ill-suited as a default local gate (matches the existing releasetest
# policy that excludes cross-arm/cross-riscv).  CI invokes it directly.
.PHONY: test-freestanding
test-freestanding: cross-arm-bytecode-only cross-riscv-bytecode-only cross-stm32f4-bytecode-only
	sh tests/scripts/test-freestanding.sh build/cross-arm-bytecode-only/liburbi.a
	sh tests/scripts/test-freestanding.sh build/cross-riscv-bytecode-only/liburbi.a
	sh tests/scripts/test-freestanding.sh build/cross-stm32f4-bytecode-only/liburbi.a

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
	# W5/v0.10.6: enforce ≥85% line coverage (Path A — see release-readiness.md §Coverage).
	# Threshold set at 85% to match measured baseline at v0.10.6-stabilization (87%).
	# Aspirational target for v1.0 is 90%; raise the threshold when the gap closes.
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

docs-check: docs-check-tools docs-public-scrub
	markdownlint-cli2 --config .markdownlint.yaml $(DOCS_LINT_TARGETS)
	@echo "--- link-check ---"
	@find docs examples components tests/qemu \
	    README.md CONTRIBUTING.md CHANGELOG.md \
	    -name '*.md' -type f \
	    ! -path '*/build/*' ! -path '*/_deps/*' \
	    -exec markdown-link-check --quiet --config .markdown-link-check.json {} +

# docs-public-scrub — verify no tracked file mentions workspace-private paths,
# tool-context filenames, or AI-attribution patterns.  Allowed exceptions must
# carry a `scrub-allow: <reason>` marker on the same line.
docs-public-scrub:
	@tests/scripts/check-public-doc-scrub.sh

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

.PHONY: all aux core test test-asan test-ubsan test-debug test-switch test-trace test-trace-compiled-out test-determinism test-determinism-default test-determinism-footprint test-determinism-linux test-determinism-trace test-perf-counters test-determinism-perf cross-arm cross-riscv cross-stm32f4 cross-pico cross-arm-bytecode-only cross-riscv-bytecode-only cross-stm32f4-bytecode-only cross-pico-bytecode-only cross-pico-repl cross-esp32s3-bytecode-only cross-esp32s3-full clean bake-clean compile_commands.json tidy tidy-fix test-tidy-strict cppcheck test-cppcheck test-scan-build analyzer lint docs-check docs-check-tools docs-public-scrub check-version-sync coverage coverage-tools test-branch-coverage test-valgrind test-valgrind-deep valgrind-tools fuzz-lex fuzz-parse fuzz-vm fuzz-build fuzz-tools urbi-bin urbi-server-bin urbi-send-bin test-integration test-urbi-server-smoke test-chk test-chk-ros releasetest _releasetest_phase1 _releasetest_phase2 test-stress test-gc-none-build test-gc-pause test-loc-cap test-docstring-coverage test-bake-smoke test-bytecode-only test-freestanding test-freestanding-host test-cross-esp32s3-freestanding-golden test-cross-pico-freestanding-golden test-cross-pico-repl-elf test-cross-stm32f4-app test-gc-roots-coverage test-api-manifest test-aux-symbols test-embedding-guide test-external-embed-iinclude oracle-diff test-port-stm32f4 test-abi-freeze test-wire-freeze test-repl-security test-stdlib-bytecode-fresh test-dependency-pins test-trace-decode test-trace-capture test-gdb test-gdb-memdebug test-mem-debug test-gc-stress test-determinism-memdebug urbi-trace unit-runner test-ros2 check-ros-gate check-rosgen check-rosgen-determinism ros-integration test-urobotics test-chk-urobotics check-urobotics-determinism test-ros-urobotics test-chk-ros-urobotics test-chk-runner test-fuzz-smoke test-o2 fuzz-json force-flagstamp
