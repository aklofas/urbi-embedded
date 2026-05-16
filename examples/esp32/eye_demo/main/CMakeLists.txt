# eye_demo main component.
#
# SRCS list grows phase-by-phase:
#   Phase 3:               eye_camera.c
#   Phase 4:               eye_display.c
#   Phase 5 (T36):         eye_button.c
#   Phase 5 (T38):         eye_demo_main.c (stub; T40 fills in app_main)

idf_component_register(
    SRCS
        "eye_camera.c"
        "eye_display.c"
        "eye_button.c"
        "eye_demo_main.c"
    INCLUDE_DIRS
        "."
        "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES
        urbi
        urbi_aux
        espressif__esp_video
        espressif__esp_cam_sensor
        freertos
        esp_timer
        esp_lcd
        driver
)

# === Bytecode pre-bake step =========================================
#
# Compile eye_demo.u to a C header containing a const uint8_t array
# via tools/urbi-compile-stdlib --to-header (added in v0.7.0 / M7 Wave 1).
# The generated header lives in the build directory and is included
# from eye_demo_main.c at T40 (extern const uint8_t eye_demo_bytecode[];
# extern const size_t eye_demo_bytecode_size;).
#
# Tool location: the worktree root sits three levels up from
# examples/esp32/eye_demo/ (this project's CMAKE_SOURCE_DIR).  The
# urbi-compile-stdlib binary is built host-side as part of the main
# urbi-embedded build; ESP-IDF cross-compilation does not rebuild it.
# If the binary is missing or stale, rebuild it from the worktree root
# with `make tools/urbi-compile-stdlib` before running idf.py.
#
# Custom-command + add_dependencies live AFTER idf_component_register
# because ESP-IDF invokes each component CMakeLists.txt twice — once in
# requirements-scripting mode where add_custom_command is forbidden, and
# once in the real build pass where ${COMPONENT_LIB} exists.  Placing
# them after idf_component_register skips the scripting pass cleanly.
set(URBI_COMPILE_TOOL "${CMAKE_SOURCE_DIR}/../../../tools/urbi-compile-stdlib")
set(BC_HEADER "${CMAKE_CURRENT_BINARY_DIR}/eye_demo_bytecode.h")

add_custom_command(
    OUTPUT  ${BC_HEADER}
    COMMAND ${URBI_COMPILE_TOOL} --to-header
            -i ${CMAKE_CURRENT_SOURCE_DIR}/eye_demo.u
            -o ${BC_HEADER}
            --symbol eye_demo_bytecode
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/eye_demo.u
            ${URBI_COMPILE_TOOL}
    COMMENT "Baking eye_demo.u -> eye_demo_bytecode.h"
    VERBATIM
)
add_custom_target(eye_demo_bytecode_h DEPENDS ${BC_HEADER})

# Ensure the bake step runs before the component library compiles —
# eye_demo_main.c #includes eye_demo_bytecode.h.
add_dependencies(${COMPONENT_LIB} eye_demo_bytecode_h)
