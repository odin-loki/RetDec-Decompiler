add_library(retdec-concurrency-detect
    concurrency_detect.cpp
)

target_include_directories(retdec-concurrency-detect
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

set_target_properties(retdec-concurrency-detect PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

install(TARGETS retdec-concurrency-detect
    EXPORT  retdec-targets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
