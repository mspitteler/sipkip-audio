# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(COMPONENT_DIR "components/esp-libopus")
set(COMPONENT_DIR "components/libglob")
set(COMPONENT_DIR "components/esp_littlefs")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(main)
project(${COMPONENT_DIR}/CMakeLists.txt)
