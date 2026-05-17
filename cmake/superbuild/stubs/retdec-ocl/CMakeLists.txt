cmake_minimum_required(VERSION 3.26)

project(retdec-ocl-stub
    LANGUAGES C CXX
    VERSION 0.1.0
    DESCRIPTION "RetDec OpenCL host layer — stub (real implementation in src/opencl/)"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

add_library(retdec-ocl STATIC
    retdec-ocl.cpp
)

target_compile_features(retdec-ocl PUBLIC cxx_std_23)

target_include_directories(retdec-ocl
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Optional: link real OpenCL if available (stubs fall through gracefully).
find_package(OpenCL QUIET)
if(OpenCL_FOUND)
    target_link_libraries(retdec-ocl PUBLIC OpenCL::OpenCL)
    target_compile_definitions(retdec-ocl PUBLIC RETDEC_OCL_AVAILABLE=1)
    message(STATUS "retdec-ocl: OpenCL found — linking real runtime")
else()
    target_compile_definitions(retdec-ocl PUBLIC RETDEC_OCL_AVAILABLE=0)
    message(STATUS "retdec-ocl: OpenCL not found — building stub only")
endif()

install(
    TARGETS retdec-ocl
    EXPORT retdec-ocl-targets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(
    FILES retdec-ocl.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/retdec"
)
