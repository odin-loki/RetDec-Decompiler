add_library(retdec_sort_detect
    partition_detect.cpp
    introsort_detect.cpp
    mergesort_detect.cpp
    heapsort_detect.cpp
    radixsort_detect.cpp
    sort_detector.cpp
)

target_include_directories(retdec_sort_detect
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(retdec_sort_detect
    PUBLIC
        retdec_ssa
)

target_compile_features(retdec_sort_detect PUBLIC cxx_std_17)

install(TARGETS retdec_sort_detect
    EXPORT  retdec-targets
    ARCHIVE DESTINATION lib
)
