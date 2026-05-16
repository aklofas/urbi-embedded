# Trivial ESP-IDF app: verifies components/urbi/ builds + links under xtensa.
#
# Only calls urbi_api_version() — does NOT exercise VM functionality. This
# is the FIRST cross-compile of the codebase with xtensa-esp-elf; build success
# proves all src/*.c files compile under ESP-IDF and the component links.
#
# Target: esp32s3. Set via `idf.py --preview set-target esp32s3` before build.

cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "../../../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(component_smoke)
