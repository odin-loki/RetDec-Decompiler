cmake_minimum_required(VERSION 3.26)

project(retdec-ml-stub
    LANGUAGES C CXX
    VERSION 0.1.0
    DESCRIPTION "RetDec Qwen3 ML inference framework — stub"
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

add_library(retdec-ml STATIC
    retdec-ml.cpp
)

target_compile_features(retdec-ml PUBLIC cxx_std_23)

target_include_directories(retdec-ml
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

install(
    TARGETS retdec-ml
    EXPORT retdec-ml-targets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(
    FILES retdec-ml.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/retdec"
)
