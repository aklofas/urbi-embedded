# The qemu smoke: the component's arena, a baked workload, the event queue
# and a session on an in-memory transport, run under `idf.py qemu` by
# run_smoke.sh against expected_markers.txt.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(reactive_smoke)
