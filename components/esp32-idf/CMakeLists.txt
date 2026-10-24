# urbi-embedded as an ESP-IDF component: the port glue in src/port/ plus
# the archive the repository Makefile builds for the xtensa-esp32s3 preset.
# The runtime is not compiled here: `make esp32s3-eye-demo` (or
# `make esp32s3-archives`) builds liburbi.a with the same toolchain idf.py
# uses, and this component links it.  A project that wants the component
# alone must build that archive first; the error below says how.
set(URBI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(LIBURBI_ARCHIVE "${URBI_ROOT}/build/xtensa-esp32s3-hosted/liburbi.a"
    CACHE FILEPATH "The archive to link (make cross-xtensa-esp32s3-hosted inside the IDF environment)")
# IDF's early requirements pass does not receive -DLIBURBI_ARCHIVE; only the real configure checks.
if (NOT CMAKE_BUILD_EARLY_EXPANSION AND NOT EXISTS "${LIBURBI_ARCHIVE}")
    message(FATAL_ERROR "${LIBURBI_ARCHIVE} is missing: run `make esp32s3-archives` at the repository root")
endif ()
file(GLOB URBI_PORT_SRC "${CMAKE_CURRENT_LIST_DIR}/src/port/*.c")
idf_component_register(
    SRCS ${URBI_PORT_SRC}
    INCLUDE_DIRS "${URBI_ROOT}/include" "${CMAKE_CURRENT_LIST_DIR}/src/port"
    REQUIRES freertos esp_timer esp_system heap driver esp_driver_usb_serial_jtag log
)
add_library(liburbi STATIC IMPORTED GLOBAL)
set_target_properties(liburbi PROPERTIES IMPORTED_LOCATION "${LIBURBI_ARCHIVE}")
# The archive reaches stderr through esp_libc's __getreent; naming the
# dependency puts esp_libc after the archive on the link line instead of
# relying on another component to pull it in.
target_link_libraries(liburbi INTERFACE idf::esp_libc)
target_link_libraries(${COMPONENT_LIB} PUBLIC liburbi)
# The port files include IDF headers that need GNU C11; the runtime archive
# itself was built -std=c99 -Wpedantic by the Makefile.
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror -mlongcalls)
