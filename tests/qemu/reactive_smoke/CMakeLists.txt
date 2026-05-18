# QEMU reactive smoke test project.
#
# Wraps the urbi managed-component at ../../../components/esp32-idf and runs
# under espressif/qemu via `idf.py qemu`.  See run_smoke.sh for the
# harness + expected_markers.txt for the assertion oracle.

cmake_minimum_required(VERSION 3.16)

# Pull in urbi from the worktree's components/ directory.  Path is relative
# to this CMakeLists.txt — tests/qemu/reactive_smoke/ sits three levels
# below the worktree root (same depth as examples/esp32/eye_demo/), so
# the components/ shim is at ../../../components.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(reactive_smoke)
