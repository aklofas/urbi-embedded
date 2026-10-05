# SPDX-License-Identifier: BSD-3-Clause
# examples/pico/repl_demo — the Raspberry Pi Pico eval-service demo.
#
# From the repository root, `make pico-repl-demo` does all of this:
#   1. make urbi-bin                        (the host binary that bakes repl_demo.u)
#   2. make cross-arm-cortex-m0plus-hosted  (the archive this firmware links)
#   3. cmake -S examples/pico/repl_demo -B examples/pico/repl_demo/build
#   4. cmake --build examples/pico/repl_demo/build
# and prints the firmware's size.  Flash build/repl_demo.uf2 by holding
# BOOTSEL while plugging the Pico in and copying the file onto RPI-RP2.
#
# The archive is the hosted shape of the Cortex-M0+ preset: newlib is
# present, so the library is built without -ffreestanding and keeps the
# value formatter and the eval service.
cmake_minimum_required(VERSION 3.13)

if (NOT DEFINED PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../../tools/pico-sdk"
        CACHE PATH "Path to the Pico SDK")
endif ()
include(pico_sdk_import.cmake)

project(urbi_pico_repl_demo C CXX ASM)
pico_sdk_init()

set(URBI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(URBI_HOST_BIN "${URBI_ROOT}/build/host/urbi"
    CACHE FILEPATH "The host urbi binary that bakes the workload (make urbi-bin)")
set(LIBURBI_ARCHIVE "${URBI_ROOT}/build/arm-cortex-m0plus-hosted/liburbi.a"
    CACHE FILEPATH "The archive to link (make cross-arm-cortex-m0plus-hosted)")
if (NOT EXISTS "${URBI_HOST_BIN}")
    message(FATAL_ERROR "${URBI_HOST_BIN} is missing: run `make urbi-bin` at the repository root")
endif ()
if (NOT EXISTS "${LIBURBI_ARCHIVE}")
    message(FATAL_ERROR "${LIBURBI_ARCHIVE} is missing: run `make cross-arm-cortex-m0plus-hosted` at the repository root")
endif ()

add_library(liburbi STATIC IMPORTED)
set_target_properties(liburbi PROPERTIES IMPORTED_LOCATION "${LIBURBI_ARCHIVE}")

# --- the workload, baked by the host urbi ------------------------------
set(BAKED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/repl_demo_baked.h")
add_custom_command(
    OUTPUT "${BAKED_HEADER}"
    COMMAND ${CMAKE_COMMAND}
            -DURBI=${URBI_HOST_BIN}
            -DIN=${CMAKE_CURRENT_SOURCE_DIR}/repl_demo.u
            -DOUT=${BAKED_HEADER}
            -DSYM=repl_demo_wire
            -P ${CMAKE_CURRENT_SOURCE_DIR}/bake.cmake
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/repl_demo.u"
            "${CMAKE_CURRENT_SOURCE_DIR}/bake.cmake"
            "${URBI_HOST_BIN}"
    COMMENT "Baking repl_demo.u"
    VERBATIM)
add_custom_target(repl_demo_bake DEPENDS "${BAKED_HEADER}")

# --- shared flags: the example includes only the public headers --------
set(REPL_DEMO_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/main"
    "${CMAKE_CURRENT_SOURCE_DIR}/main/bsp"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${URBI_ROOT}/include")
set(REPL_DEMO_SDK_LIBS
    pico_stdlib
    tinyusb_device
    tinyusb_board
    hardware_adc
    hardware_pwm
    hardware_irq
    hardware_timer
    hardware_sync
    hardware_gpio
    hardware_uart)

# --- the firmware -------------------------------------------------------
# One executable holds every file.  The SDK's libraries are INTERFACE
# libraries that carry their sources, so any second target linking them
# would compile the SDK again and the link would see each SDK object
# twice; they are therefore linked once, here.  The warning flags go on
# the example's own files only, so the SDK's sources are not built with
# -Werror.
set(REPL_DEMO_SOURCES
    main/main.c
    main/usb_descriptors.c
    main/transport_usb_cdc.c
    main/transport_uart.c
    main/bsp/bsp_register.c
    main/bsp/bsp_led.c
    main/bsp/bsp_temp.c
    main/bsp/bsp_button.c
    main/bsp/bsp_tick.c)
set_source_files_properties(${REPL_DEMO_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
add_executable(repl_demo
    ${REPL_DEMO_SOURCES}
    "${BAKED_HEADER}")
add_dependencies(repl_demo repl_demo_bake)
target_include_directories(repl_demo PRIVATE ${REPL_DEMO_INCLUDES})
# The SDK's newlib init installs a no-access MPU region at __StackBottom
# (memmap_repl_demo.ld), so a stack overrun faults instead of writing
# into the heap; main.c routes the fault to its error pattern.
target_compile_definitions(repl_demo PRIVATE PICO_USE_STACK_GUARDS=1)
target_link_libraries(repl_demo PRIVATE
    liburbi
    ${REPL_DEMO_SDK_LIBS})

# The stack lives at the top of main SRAM (see memmap_repl_demo.ld); the
# included default map and the SDK's generated flash fragment are found
# through these two search paths.
pico_set_linker_script(repl_demo "${CMAKE_CURRENT_SOURCE_DIR}/memmap_repl_demo.ld")
target_link_options(repl_demo PRIVATE
    "LINKER:-L${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2040"
    "LINKER:-L${CMAKE_BINARY_DIR}")

# The example drives USB CDC and UART0 itself through the eval service's
# transport vtable; the SDK's own stdio must not own either.
pico_enable_stdio_usb(repl_demo 0)
pico_enable_stdio_uart(repl_demo 0)

pico_add_extra_outputs(repl_demo)
