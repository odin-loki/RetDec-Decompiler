cmake_minimum_required(VERSION 3.15)

add_library(retdec-serial-detect STATIC
    serial_detect.cpp
)

target_include_directories(retdec-serial-detect
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set_target_properties(retdec-serial-detect PROPERTIES
    CXX_STANDARD          17
    CXX_STANDARD_REQUIRED YES
)

install(TARGETS retdec-serial-detect
    EXPORT  retdec-targets
    ARCHIVE DESTINATION lib
)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/retdec/serial_detect
    DESTINATION include/retdec
)
