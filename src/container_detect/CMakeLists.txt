add_library(retdec_container_detect
    vector_detect.cpp
    list_detect.cpp
    map_detect.cpp
    unordered_detect.cpp
    string_detect.cpp
    container_detector.cpp
)

target_include_directories(retdec_container_detect
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(retdec_container_detect
    PUBLIC
        retdec_ssa
)

set_target_properties(retdec_container_detect PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

install(TARGETS retdec_container_detect
    EXPORT  retdec-targets
    ARCHIVE DESTINATION lib
)
