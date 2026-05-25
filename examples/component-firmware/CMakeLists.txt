cmake_minimum_required(VERSION 3.16)

get_filename_component(TINYBLOK_REPO "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

# Tinyblok supplies app_main, Wi-Fi/setup, NATS, timers, and patchbay runtime.
# This project supplies only its patchbay file and app-specific native symbols.
set(EXTRA_COMPONENT_DIRS "${TINYBLOK_REPO}/main")
set(TINYBLOK_PATCHBAY_SOURCE "patchbay.yml" CACHE FILEPATH "")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tinyblok_component_firmware)

