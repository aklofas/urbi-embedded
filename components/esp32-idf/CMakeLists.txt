# urbi-embedded as an ESP-IDF managed component.
#
# Mirrors the host Makefile SRC variable: globs every src/ subdir that
# contributes to liburbi.a (core), excludes src/urbi_aux.c (separate
# liburbi_aux.a archive — embedders opt in independently via the aux
# header / extra link), and excludes test code.
#
# Port glue under components/esp32-idf/src/port/ lands incrementally in Phase 2
# of the v0.7.2-esp32 plan (T14-T19).  The glob below picks up new port_*.c
# files as they drop into place — subsequent tasks only add files, not
# CMakeLists.txt entries.

set(URBI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

# Root src/ — everything except the aux TU (which lives in its own archive).
file(GLOB URBI_SRC_ROOT "${URBI_ROOT}/src/*.c")
list(REMOVE_ITEM URBI_SRC_ROOT "${URBI_ROOT}/src/urbi_aux.c")

# Per-subsystem globs (mirror Makefile §SRC).
file(GLOB URBI_SRC_LEX     "${URBI_ROOT}/src/lex/*.c")
file(GLOB URBI_SRC_PARSE   "${URBI_ROOT}/src/parse/*.c")
file(GLOB URBI_SRC_EMIT    "${URBI_ROOT}/src/emit/*.c")
file(GLOB URBI_SRC_VM      "${URBI_ROOT}/src/vm/*.c")
file(GLOB URBI_SRC_GC      "${URBI_ROOT}/src/gc/*.c")
file(GLOB URBI_SRC_SCHED   "${URBI_ROOT}/src/sched/*.c")
file(GLOB URBI_SRC_WATCHER "${URBI_ROOT}/src/watcher/*.c")
file(GLOB URBI_SRC_EVENT   "${URBI_ROOT}/src/event/*.c")
file(GLOB URBI_SRC_TAG     "${URBI_ROOT}/src/tag/*.c")
file(GLOB URBI_SRC_CHANGED "${URBI_ROOT}/src/changed/*.c")
file(GLOB URBI_SRC_MODULE  "${URBI_ROOT}/src/chunk/*.c")
file(GLOB URBI_SRC_VALUE   "${URBI_ROOT}/src/value/*.c")
file(GLOB URBI_SRC_RUNTIME "${URBI_ROOT}/src/runtime/*.c")
file(GLOB URBI_SRC_REALM   "${URBI_ROOT}/src/realm/*.c")
file(GLOB URBI_SRC_OBJECT  "${URBI_ROOT}/src/object/*.c")
file(GLOB URBI_SRC_STDLIB  "${URBI_ROOT}/src/stdlib/*.c")

# ESP-IDF port glue (this component's own src/port/ — not under URBI_ROOT).
file(GLOB URBI_SRC_PORT    "${CMAKE_CURRENT_SOURCE_DIR}/src/port/*.c")

set(SOURCES
    ${URBI_SRC_ROOT}
    ${URBI_SRC_LEX}
    ${URBI_SRC_PARSE}
    ${URBI_SRC_EMIT}
    ${URBI_SRC_VM}
    ${URBI_SRC_GC}
    ${URBI_SRC_SCHED}
    ${URBI_SRC_WATCHER}
    ${URBI_SRC_EVENT}
    ${URBI_SRC_TAG}
    ${URBI_SRC_CHANGED}
    ${URBI_SRC_MODULE}
    ${URBI_SRC_VALUE}
    ${URBI_SRC_RUNTIME}
    ${URBI_SRC_REALM}
    ${URBI_SRC_OBJECT}
    ${URBI_SRC_STDLIB}
    ${URBI_SRC_PORT}
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS
        "${URBI_ROOT}/include"
        "${URBI_ROOT}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/port"
    REQUIRES freertos esp_timer esp_system heap
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=c99 -Wall -Wextra -mlongcalls  # NOTE: -ffreestanding removed; gates urbi_module_from_bytes + stdlib_boot via __STDC_HOSTED__ on hosted ESP-IDF newlib path
    # -Wpedantic is intentionally OFF for the urbi component on ESP-IDF.  Three
    # sources of unfixable noise flood the build log otherwise (hundreds of
    # lines, drowning serial output during on-device debugging):
    #   1. _Static_assert (C11, not C99) — used everywhere for layout pins
    #   2. #include_next — from ESP-IDF's OWN platform_include/{assert,stdio,time}.h
    #   3. computed goto + address-of-label — deliberate perf opt in src/vm/uvm.c
    # Host Makefile keeps -Wpedantic on (without -Werror) for new-code review,
    # but the IDF build path treats it as cosmetic noise.
    -Wno-pedantic
)

# Port-glue TUs that include ESP-IDF headers (esp_log.h, esp_timer.h, etc.)
# need IDF's default language standard (gnu23) because those headers use
# C11+ _Static_assert and GNU unnamed-union extensions that the urbi core's
# -std=c99 -Wpedantic rejects with hard parse errors.
#
# We override -std=c99 on a per-source basis for the port wrappers; the rest
# of the urbi component (core C99 sources) keeps the strict mode above.
set_source_files_properties(
    ${URBI_SRC_PORT}
    PROPERTIES COMPILE_OPTIONS "-std=gnu11;-Wno-pedantic"
)
