idf_component_register(
    SRCS "reactive_smoke_main.c" "smoke_transport.c"
    INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES esp32-idf freertos esp_timer
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
set(BAKED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/reactive_smoke_baked.h")
add_custom_command(
    OUTPUT "${BAKED_HEADER}"
    COMMAND ${CMAKE_COMMAND} -DURBI=${URBI_HOST_BIN} -DIN=${CMAKE_CURRENT_LIST_DIR}/reactive_smoke.u
            -DOUT=${BAKED_HEADER} -DSYM=reactive_smoke_wire
            -P ${URBI_ROOT}/components/esp32-idf/bake.cmake
    DEPENDS "${CMAKE_CURRENT_LIST_DIR}/reactive_smoke.u" "${URBI_ROOT}/components/esp32-idf/bake.cmake" "${URBI_HOST_BIN}"
    COMMENT "Baking reactive_smoke.u"
    VERBATIM)
add_custom_target(reactive_smoke_bake DEPENDS "${BAKED_HEADER}")
add_dependencies(${COMPONENT_LIB} reactive_smoke_bake)
