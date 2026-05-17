add_executable(retdec-profiling-tests
    profiling_test.cpp
)

target_include_directories(retdec-profiling-tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(retdec-profiling-tests
    PRIVATE
        retdec-profiling
        retdec::deps::gtest
        retdec::deps::gtest_main
)

if(WIN32)
    target_link_libraries(retdec-profiling-tests PRIVATE psapi)
endif()

set_target_properties(retdec-profiling-tests PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME retdec-profiling-tests
    COMMAND retdec-profiling-tests
)
