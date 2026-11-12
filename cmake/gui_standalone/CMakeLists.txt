# Builds retdec-gui and its tests without the rest of RetDec (no LLVM, no
# ExternalProjects): FuseUI + Forge from third_party/ and the system gtest.
# retdec-gui only talks to the decompiler as a child process, so this is the
# whole GUI.
#
#   cmake -S cmake/gui_standalone -B build/gui -G Ninja
#   cmake --build build/gui && ctest --test-dir build/gui --output-on-failure
cmake_minimum_required(VERSION 3.25)
project(retdec_gui_standalone LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(APPLE)
	enable_language(OBJCXX)
endif()
enable_testing()

get_filename_component(RETDEC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(GTest REQUIRED)
if(NOT TARGET retdec::deps::gtest)
	add_library(retdec::deps::gtest ALIAS GTest::gtest)
	add_library(retdec::deps::gtest_main ALIAS GTest::gtest_main)
endif()

set(RETDEC_GUI ON)
include("${RETDEC_ROOT}/cmake/first_party.cmake")
add_subdirectory("${RETDEC_ROOT}/src/gui" "${CMAKE_BINARY_DIR}/gui")
add_subdirectory("${RETDEC_ROOT}/tests/gui" "${CMAKE_BINARY_DIR}/gui-tests")
