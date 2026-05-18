# ESP32-S3-EYE color blob tracking demo.
#
# Builds against the urbi managed-component at ../../../components/esp32-idf and
# pulls espressif/esp32-camera from the ESP Component Registry via
# main/idf_component.yml.

cmake_minimum_required(VERSION 3.16)

# Pull in urbi from the worktree's components/ directory.  Path is relative
# to this CMakeLists.txt (examples/esp32/eye_demo/ → ../../../components).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(eye_demo)
