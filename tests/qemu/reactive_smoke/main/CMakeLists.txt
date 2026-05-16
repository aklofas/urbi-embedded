# reactive_smoke main component.

idf_component_register(
    SRCS
        "reactive_smoke_main.c"
    INCLUDE_DIRS
        "."
        "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES
        urbi
        freertos
        esp_timer
)

# === Bytecode pre-bake step =========================================
#
# Same pattern as examples/esp32/eye_demo/main/CMakeLists.txt: compile
# reactive_smoke.u to a C header via tools/urbi-compile-stdlib --to-header
# (added in v0.7.0 / M7 Wave 1).  The generated header lives in the build
# directory and is included from reactive_smoke_main.c.
#
# tests/qemu/reactive_smoke/ sits three levels below the worktree root —
# same depth as examples/esp32/eye_demo/ — so the tool path also walks
# three levels up from CMAKE_SOURCE_DIR.
#
# Custom-command + add_dependencies live AFTER idf_component_register
# because ESP-IDF invokes each component CMakeLists.txt twice — once in
# requirements-scripting mode (add_custom_command forbidden), and once in
# the real build pass where ${COMPONENT_LIB} exists.
set(URBI_COMPILE_TOOL "${CMAKE_SOURCE_DIR}/../../../tools/urbi-compile-stdlib")
set(BC_HEADER "${CMAKE_CURRENT_BINARY_DIR}/reactive_smoke_bytecode.h")

add_custom_command(
    OUTPUT  ${BC_HEADER}
    COMMAND ${URBI_COMPILE_TOOL} --to-header
            -i ${CMAKE_CURRENT_SOURCE_DIR}/reactive_smoke.u
            -o ${BC_HEADER}
            --symbol reactive_smoke_bytecode
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/reactive_smoke.u
            ${URBI_COMPILE_TOOL}
    COMMENT "Baking reactive_smoke.u -> reactive_smoke_bytecode.h"
    VERBATIM
)
add_custom_target(reactive_smoke_bytecode_h DEPENDS ${BC_HEADER})

# Ensure the bake step runs before the component library compiles —
# reactive_smoke_main.c #includes reactive_smoke_bytecode.h.
add_dependencies(${COMPONENT_LIB} reactive_smoke_bytecode_h)
