# CMakeLists.txt for Echoes with OTA

cmake_minimum_required(VERSION 3.16)

# Include ESP-IDF CMake configuration
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(PARTITION_TABLE_CSV_PATH ${CMAKE_SOURCE_DIR}/partitions.csv)

# Define project
project(echoes)
