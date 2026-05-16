# urbi_aux — optional convenience layer for urbi-embedded.
#
# Mirrors the host Makefile's separate liburbi_aux.a archive per the
# v0.7.0 governance policy (CONTRIBUTING.md "Aux layer governance"):
# every aux function must be strictly implementable via <urbi/urbi.h>
# public API — no private header access, no internal state peeking, no
# performance shortcuts.  The separate component enforces this
# physically — only `${URBI_ROOT}/include` is on the include path, so
# aux code cannot reach into src/.
#
# Embedders opt in by listing `urbi_aux` in their component's REQUIRES.
# Default ESP-IDF builds get `urbi` only — zero aux symbols, smallest
# footprint, no link-time cost.
#
# Mental model: matches Lua's lua.h (core) vs lauxlib.h (auxlib) split.

set(URBI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

idf_component_register(
    SRCS "${URBI_ROOT}/src/urbi_aux.c"
    INCLUDE_DIRS "${URBI_ROOT}/include"
    REQUIRES urbi
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=c99 -Wall -Wextra -mlongcalls
    # -Wpedantic intentionally OFF for the same reasons as components/urbi/:
    # _Static_assert, #include_next noise from ESP-IDF platform_include/,
    # and computed goto in urbi internals.  See components/urbi/CMakeLists.txt
    # for the full rationale.
    -Wno-pedantic
)
