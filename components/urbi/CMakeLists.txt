# urbi-embedded as an ESP-IDF managed component.
#
# Mirrors the host Makefile SRC variable: globs every src/ subdir that
# contributes to liburbi.a (core), excludes src/urbi_aux.c (separate
# liburbi_aux.a archive — embedders opt in independently via the aux
# header / extra link), and excludes test code.
#
# Port glue under src/port/ lands incrementally in Phase 2 of the
# v0.7.2-esp32 plan (T14-T19); add those entries here as each file
# ships. Until then the component compiles the platform-neutral core.

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
file(GLOB URBI_SRC_MODULE  "${URBI_ROOT}/src/module/*.c")
file(GLOB URBI_SRC_VALUE   "${URBI_ROOT}/src/value/*.c")
file(GLOB URBI_SRC_RUNTIME "${URBI_ROOT}/src/runtime/*.c")
file(GLOB URBI_SRC_REALM   "${URBI_ROOT}/src/realm/*.c")
file(GLOB URBI_SRC_OBJECT  "${URBI_ROOT}/src/object/*.c")
file(GLOB URBI_SRC_STDLIB  "${URBI_ROOT}/src/stdlib/*.c")

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
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "${URBI_ROOT}/include" "${URBI_ROOT}/src"
    REQUIRES freertos esp_timer esp_system heap
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=c99 -Wall -Wextra -Wpedantic -ffreestanding -mlongcalls
    # ESP-IDF defaults to -Werror; the urbi sources use _Static_assert (a GCC
    # extension under C99, standard in C11). Keep the diagnostic visible as a
    # warning but don't fatal-fail the build. The host Makefile uses -Wpedantic
    # without -Werror and so accepts these silently.
    -Wno-error=pedantic
)
