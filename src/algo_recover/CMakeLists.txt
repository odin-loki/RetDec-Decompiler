add_library(retdec_algo_recover
    transform_detect.cpp
    accumulate_detect.cpp
    find_detect.cpp
    partition_detect.cpp
    foreach_detect.cpp
    iterator_recover.cpp
    algo_detector.cpp
)

target_include_directories(retdec_algo_recover
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(retdec_algo_recover
    PUBLIC
        retdec_ssa
)

set_target_properties(retdec_algo_recover PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

install(TARGETS retdec_algo_recover
    EXPORT  retdec-targets
    ARCHIVE DESTINATION lib
)
