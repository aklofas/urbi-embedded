# SPDX-License-Identifier: BSD-3-Clause
# examples/pico/repl_demo/CMakeLists.txt
#
# Build the v0.9.4 Pi Pico interactive REPL demo (.uf2).
#
# Prerequisites (run from the repo root):
#   1. tools/pico-sdk/  — vendored Pico SDK (see tools/setup-pico-sdk.sh)
#   2. arm-none-eabi-gcc toolchain on $PATH
#   3. liburbi.a built with REPL + USB CDC support:
#         make cross-pico URBI_ENABLE_REPL=1
#      Output lands at build/arm-cortex-m0plus/liburbi.a (the IMPORTED
#      target below).
#   4. tools/urbi-compile-stdlib-pico  (built automatically by step 3's
#      recipe transitively, or `make tools/urbi-compile-stdlib-pico`).
#
# Then:
#   cd examples/pico/repl_demo
#   mkdir build && cd build
#   cmake ..
#   make
#
# Flash repl_demo.uf2 onto the Pico via BOOTSEL.

cmake_minimum_required(VERSION 3.13)

# PICO_SDK_PATH precedence (pico_sdk_import.cmake handles this):
#   1. -DPICO_SDK_PATH=... on cmake invocation
#   2. $PICO_SDK_PATH env var
#   3. The default below — workspace-root tools/pico-sdk (four levels up
#      from this CMakeLists.txt, alongside urbi-embedded/ itself, per the
#      v0.9.4 setup convention).  Embedders outside that layout MUST
#      override one of (1)/(2).
if (NOT DEFINED PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../../tools/pico-sdk"
        CACHE PATH "Path to the Pico SDK")
endif ()
include(pico_sdk_import.cmake)

project(urbi_pico_repl_demo C CXX ASM)
pico_sdk_init()

# --- liburbi.a (cross-built by `make cross-pico URBI_ENABLE_REPL=1`) ---
# LIBURBI_BUILD_SUBDIR may be overridden on the cmake invocation to point at
# an alternate build directory (e.g. arm-cortex-m0plus-repl for the
# cross-pico-repl cooperative-REPL build used by test-cross-pico-repl-elf).
if (NOT DEFINED LIBURBI_BUILD_SUBDIR)
    set(LIBURBI_BUILD_SUBDIR "arm-cortex-m0plus")
endif ()
add_library(liburbi STATIC IMPORTED)
set_target_properties(liburbi PROPERTIES
    IMPORTED_LOCATION
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../build/${LIBURBI_BUILD_SUBDIR}/liburbi.a)

# --- Bake repl_demo.u into repl_demo_baked.h at configure/build time. ---
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/repl_demo_baked.h
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/urbi-compile-stdlib-pico
            --to-header
            -i ${CMAKE_CURRENT_SOURCE_DIR}/repl_demo.u
            -o ${CMAKE_CURRENT_BINARY_DIR}/repl_demo_baked.h
            --symbol repl_demo
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/repl_demo.u
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/urbi-compile-stdlib-pico
    COMMENT "Baking repl_demo.u into repl_demo_baked.h"
    VERBATIM)

# --- Pi Pico REPL transport sources (gated empty TUs inside liburbi.a
#     when cross-pico isn't built with URBI_PICO_USB_CDC; pulled in here
#     directly so the example's URBI_PICO_USB_CDC=1 define lights them
#     up against the example's tinyusb + pico-sdk include paths). ---
set(URBI_PICO_REPL_TRANSPORT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/repl/urepl_transport_usb_cdc_pico.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src/repl/urepl_transport_uart_pico.c)

# --- The demo executable. ---
add_executable(repl_demo
    main/main.c
    main/usb_descriptors.c
    main/bsp/bsp_register.c
    main/bsp/bsp_led.c
    main/bsp/bsp_temp.c
    main/bsp/bsp_button.c
    main/bsp/bsp_tick.c
    ${URBI_PICO_REPL_TRANSPORT_SOURCES}
    ${CMAKE_CURRENT_BINARY_DIR}/repl_demo_baked.h)

target_include_directories(repl_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/main
    ${CMAKE_CURRENT_SOURCE_DIR}/main/bsp
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../src)

target_link_libraries(repl_demo PRIVATE
    liburbi
    pico_stdlib
    pico_stdio_usb
    pico_unique_id
    tinyusb_device
    tinyusb_board
    hardware_adc
    hardware_pwm
    hardware_irq
    hardware_timer
    hardware_sync
    hardware_gpio)

target_compile_definitions(repl_demo PRIVATE
    URBI_ENABLE_REPL=1
    URBI_PICO_USB_CDC=1
    URBI_FLOAT_TYPE=4)

# We route REPL traffic over USB CDC + UART ourselves through urbi's
# UTransport vtable; disable pico-sdk's own stdio-over-{usb,uart} so
# its CDC ISR doesn't fight ours.
pico_enable_stdio_usb(repl_demo 0)
pico_enable_stdio_uart(repl_demo 0)

# Generates the .uf2 + .hex + .bin alongside the .elf.
pico_add_extra_outputs(repl_demo)
