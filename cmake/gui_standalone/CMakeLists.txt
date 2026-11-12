# Builds retdec-gui and its tests without the rest of RetDec (no LLVM, no
# ExternalProjects): FuseUI + Forge from third_party/ and the system gtest.
# retdec-gui only talks to the decompiler as a child process, so this is the
# whole GUI.
#
#   cmake -S cmake/gui_standalone -B build/gui -G Ninja
#   cmake --build build/gui && ctest --test-dir build/gui --output-on-failure
#
# GoogleTest: find_package(GTest) first (libgtest-dev, brew googletest, a vcpkg
# install via GTest_ROOT / CMAKE_PREFIX_PATH). When there is none and
# RETDEC_GUI_FETCH_GTEST is ON (the default) the googletest release pinned in
# cmake/deps.cmake is fetched instead -- that is how MSVC runners get it.
# RETDEC_GUI_STANDALONE_TESTS=OFF builds only retdec-gui (the superbuild's
# GUI-only path).
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
option(RETDEC_GUI_STANDALONE_TESTS "Build retdec-gui-tests" ON)
option(RETDEC_GUI_FETCH_GTEST "Fetch googletest when find_package(GTest) finds none" ON)
if(RETDEC_GUI_STANDALONE_TESTS)
	find_package(GTest QUIET)
	if(GTest_FOUND)
		add_library(retdec_gui_gtest INTERFACE)
		target_link_libraries(retdec_gui_gtest INTERFACE GTest::gtest)
		add_library(retdec_gui_gtest_main INTERFACE)
		target_link_libraries(retdec_gui_gtest_main INTERFACE GTest::gtest_main)
	elseif(RETDEC_GUI_FETCH_GTEST)
		include("${RETDEC_ROOT}/cmake/deps.cmake")
		message(STATUS "retdec-gui: no system GoogleTest; fetching ${GOOGLETEST_URL}")
		include(FetchContent)
		# Match the MSVC runtime the rest of this tree uses (/MD).
		set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
		set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
		set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
		FetchContent_Declare(googletest
			URL "${GOOGLETEST_URL}"
			URL_HASH "SHA256=${GOOGLETEST_ARCHIVE_SHA256}"
			DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
		FetchContent_MakeAvailable(googletest)
		add_library(retdec_gui_gtest INTERFACE)
		target_link_libraries(retdec_gui_gtest INTERFACE gtest)
		add_library(retdec_gui_gtest_main INTERFACE)
		target_link_libraries(retdec_gui_gtest_main INTERFACE gtest_main)
	else()
		message(FATAL_ERROR "retdec-gui-tests needs GoogleTest: install it (libgtest-dev, "
			"brew googletest, vcpkg gtest), set RETDEC_GUI_FETCH_GTEST=ON, or set "
			"RETDEC_GUI_STANDALONE_TESTS=OFF")
	endif()
	if(NOT TARGET retdec::deps::gtest)
		add_library(retdec::deps::gtest ALIAS retdec_gui_gtest)
		add_library(retdec::deps::gtest_main ALIAS retdec_gui_gtest_main)
	endif()
endif()

set(RETDEC_GUI ON)
include("${RETDEC_ROOT}/cmake/first_party.cmake")
add_subdirectory("${RETDEC_ROOT}/src/gui" "${CMAKE_BINARY_DIR}/gui")
if(RETDEC_GUI_STANDALONE_TESTS)
	add_subdirectory("${RETDEC_ROOT}/tests/gui" "${CMAKE_BINARY_DIR}/gui-tests")
endif()
