cmake_minimum_required(VERSION 3.20)
project(retdec-decompiler-plugin-sample LANGUAGES CXX)

add_library(retdec_plugin_probe SHARED plugin_load_probe.cpp)
set_target_properties(retdec_plugin_probe PROPERTIES
    OUTPUT_NAME "retdec_plugin_probe"
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

if(WIN32)
    set_target_properties(retdec_plugin_probe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endif()

# Standalone sample — not part of the main RetDec superbuild unless added explicitly.
message(STATUS "retdec_plugin_probe: build shared library, then load with ctypes or future --load-pass-plugin")
