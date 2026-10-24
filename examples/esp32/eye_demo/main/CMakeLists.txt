idf_component_register(
    SRCS "eye_camera.c" "eye_display.c" "eye_button.c" "eye_demo_main.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES esp32-idf espressif__esp_video espressif__esp_cam_sensor freertos esp_timer esp_lcd driver
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
# The workload is compiled on the host at build time and baked into a
# header the firmware loads with urbi_load.
set(URBI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")
set(URBI_HOST_BIN "${URBI_ROOT}/build/host/urbi"
    CACHE FILEPATH "The host urbi binary that bakes the workload (make urbi-bin)")
if (NOT EXISTS "${URBI_HOST_BIN}")
    message(FATAL_ERROR "${URBI_HOST_BIN} is missing: run `make urbi-bin` at the repository root")
endif ()
set(BAKED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/eye_demo_baked.h")
add_custom_command(
    OUTPUT "${BAKED_HEADER}"
    COMMAND ${CMAKE_COMMAND} -DURBI=${URBI_HOST_BIN} -DIN=${CMAKE_CURRENT_LIST_DIR}/eye_demo.u
            -DOUT=${BAKED_HEADER} -DSYM=eye_demo_wire
            -P ${URBI_ROOT}/components/esp32-idf/bake.cmake
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/eye_demo.u" "${URBI_ROOT}/components/esp32-idf/bake.cmake" "${URBI_HOST_BIN}"
    COMMENT "Baking eye_demo.u"
    VERBATIM)
add_custom_target(eye_demo_bake DEPENDS "${BAKED_HEADER}")
add_dependencies(${COMPONENT_LIB} eye_demo_bake)
