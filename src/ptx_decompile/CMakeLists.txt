add_library(retdec-ptx-decompile
    cuda_host_recover.cpp
    ptx_lifter.cpp
)

target_include_directories(retdec-ptx-decompile
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set_target_properties(retdec-ptx-decompile PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

install(TARGETS retdec-ptx-decompile
    EXPORT  retdec-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
